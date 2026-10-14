"""Actual page load/publication block versus repeated physical lookup."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'kernel/fs/vfs/page_cache.c').read_text()
block=source[source.index('    page->owner = PAGE_OWNER_FILE;'):
             source.index('    *out = page;',source.index('    page->owner = PAGE_OWNER_FILE;'))]
old=block.replace('    paddr_t physical = page_to_phys(page);\n', '').replace(
    'phys_to_direct(physical)', 'phys_to_direct(page_to_phys(page))').replace(
    'physical.value + PAGE_SIZE', 'page_to_phys(page).value + PAGE_SIZE')
assert old!=block
prefix=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#define PAGE_SIZE 4096U
#define PAGE_OWNER_FILE 3U
#define PAGE_COMPOUND_TAIL 2U
#define K_OK 0
#define K_EIO -5
typedef int kstatus_t;
typedef struct {uint64_t value;} paddr_t;
typedef struct page {unsigned owner,flags;union {struct {void *mapping;uint64_t index;} file;} u;} page_t;
typedef struct entry {uint64_t index;page_t *page;bool dirty;struct entry *next,*hash_next;} vfs_cached_page_t;
typedef struct {vfs_cached_page_t *head,*bucket;unsigned entry_count;} vfs_page_cache_t;
typedef struct {uint64_t size;void *context;
 kstatus_t (*read)(void *,uint64_t,void *,size_t,uint64_t *);} backend_t;
static page_t pages[2];static vfs_cached_page_t entries[2];static vfs_page_cache_t storage;
static unsigned queries,tail_queries,reads,frees,entry_frees,unlocks,grows,mode,bad_tail,held;
static uint8_t arena[8192+128];static unsigned node;
static paddr_t paddr_make(uint64_t p){return (paddr_t){p};}
static paddr_t page_to_phys(page_t *p){assert(p==pages);++queries;return paddr_make(0x10000);}
static uint8_t *phys_to_direct(paddr_t p){assert(p.value==0x10000);return arena+64;}
static page_t *phys_to_page(paddr_t p){assert(p.value==0x11000);++tail_queries;return bad_tail==1?0:&pages[1];}
static void page_free(page_t *p){assert(p==pages);++frees;}
static void kfree(void *p){if(p){assert(p==entries||p==entries+1);++entry_frees;}}
static void vfs_vnode_unlock(void *p){assert(p==&node&&!held);++unlocks;}
static void vfs_backend_lock(backend_t *p){assert(p&&!held);held=1;}
static void vfs_backend_unlock(backend_t *p){assert(p&&held);held=0;}
static kstatus_t read_data(void *p,uint64_t offset,void *out,size_t n,uint64_t *bytes){
 (void)p;assert(held&&out==arena+64&&n<=8192);++reads;
 for(size_t i=0;i<n;++i)((uint8_t*)out)[i]=(uint8_t)(offset+i*17);
 *bytes=mode==1?n-1:mode==2?n+1:n;return mode==3?-12:K_OK;
}
static void vfs_page_cache_insert(vfs_page_cache_t *c,vfs_cached_page_t *e){
 e->next=c->head;e->hash_next=c->bucket;c->head=c->bucket=e;++c->entry_count;
}
static uint32_t vfs_page_cache_bucket(uint64_t i,uint32_t n){(void)i;assert(n==1);return 0;}
static uint32_t vfs_page_cache_bucket_count(vfs_page_cache_t *c){assert(c==&storage);return 1;}
static vfs_cached_page_t **vfs_page_cache_buckets(vfs_page_cache_t *c){return &c->bucket;}
static void vfs_page_cache_grow(vfs_page_cache_t *c){assert(c==&storage);++grows;}
'''
def wrapper(name,body):
    return 'static kstatus_t '+name+r'''(uint64_t length,bool prefetch) {
 void *vnode=&node;vfs_page_cache_t *cache=&storage;
 page_t *page=pages;vfs_cached_page_t *entry=entries,*next_entry=prefetch?entries+1:0;
 uint64_t page_index=7,next_index=8,file_offset=7*PAGE_SIZE,bytes=0,read_length;
 backend_t data={file_offset+length,0,read_data},*backend=&data;
'''+body+'return K_OK;\n}\n'
cases=r'''
typedef struct {page_t pages[2];vfs_cached_page_t entries[2];vfs_page_cache_t cache;
 unsigned tail_queries,reads,frees,entry_frees,unlocks,grows;int status;uint8_t memory[sizeof(arena)];} snapshot_t;
static void reset(void){
 memset(pages,0,sizeof(pages));memset(entries,0,sizeof(entries));memset(&storage,0,sizeof(storage));
 memset(arena,0x75,sizeof(arena));pages[1].flags=bad_tail==2?0:PAGE_COMPOUND_TAIL;
 queries=tail_queries=reads=frees=entry_frees=unlocks=grows=held=0;
}
static void capture(snapshot_t *s,int status){
 memset(s,0,sizeof(*s));memcpy(s->pages,pages,sizeof(pages));memcpy(s->entries,entries,sizeof(entries));
 s->cache=storage;s->tail_queries=tail_queries;s->reads=reads;s->frees=frees;
 s->entry_frees=entry_frees;s->unlocks=unlocks;s->grows=grows;s->status=status;
 memcpy(s->memory,arena,sizeof(arena));assert(!held);
}
int main(void){
 unsigned cases=0,reduced=0;const uint64_t lengths[]={0,1,37,4095,4096,4097,8191,8192,8193,UINT64_MAX-7*PAGE_SIZE};
 for(unsigned i=0;i<sizeof(lengths)/sizeof(lengths[0]);++i)
 for(unsigned prefetch=0;prefetch<2;++prefetch)
 for(mode=0;mode<4;++mode)for(bad_tail=0;bad_tail<3;++bad_tail){
  snapshot_t before,after;reset();int old_status=previous_load(lengths[i],prefetch);
  unsigned old_queries=queries;capture(&before,old_status);
  reset();int status=current_load(lengths[i],prefetch);capture(&after,status);
  assert(!memcmp(&before,&after,sizeof(before))&&queries==1);
  assert(old_queries==1+tail_queries);reduced+=old_queries!=queries;
  for(unsigned j=0;j<64;++j)assert(arena[j]==0x75&&arena[8192+64+j]==0x75);
  ++cases;
 }
 assert(cases==240&&reduced==15);
 printf("File physical lookup: %u exact old/new data/cache/ownership/error cases; prefetch queries 2 -> 1, tail validation and rollback preserved\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-file-physical-') as directory:
    binary=str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie','-x','c','-','-o',binary],
                   input=prefix+wrapper('previous_load',old)+wrapper('current_load',block)+cases,
                   text=True,check=True)
    subprocess.run([binary],check=True)
