"""Production cache update/writeback: partial writes must retain mmap dirt."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/fs/vfs/page_cache.c').read_text()
lookup = source[source.index('static uint32_t vfs_page_cache_bucket('):
                source.index('static void vfs_vm_mapping_retain(')]
flush = source[source.index('static kstatus_t vfs_page_cache_flush_entry('):
               source.index('static kstatus_t vfs_page_cache_flush_locked(')]
copy_in = source[source.index('void vfs_page_cache_copy_in('):]
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PAGE_SIZE 4096U
#define VFS_PAGE_CACHE_BUCKETS 256U
typedef int kstatus_t;
enum {K_OK, K_EINVAL, K_EACCES, K_EIO};
typedef struct entry {uint64_t index;void *page;bool dirty;
 struct entry *next,*hash_next;} vfs_cached_page_t;
typedef struct {vfs_cached_page_t *head,*buckets[256],**expanded_buckets;
 uint32_t bucket_count;uint64_t entry_count,growth_retry_at;} vfs_page_cache_t;
typedef struct {void *context;int (*write)(void*,uint64_t,void*,size_t,uint64_t*);} vfs_backend_node_t;
typedef struct {void *fs_private,*page_cache;uint64_t size;} vnode_t;
static unsigned vnode_depth,backend_depth,fail,short_write,writes;
static uint8_t disk[3*PAGE_SIZE];
static void *kzalloc(size_t n,int flags) {(void)flags;return calloc(1,n);}
static void kfree(void *p) {free(p);}
static void vfs_vnode_lock(vnode_t *v) {(void)v;assert(!vnode_depth);++vnode_depth;}
static void vfs_vnode_unlock(vnode_t *v) {(void)v;assert(vnode_depth==1);--vnode_depth;}
static void vfs_backend_lock(void *v) {(void)v;assert(vnode_depth==1 && !backend_depth);++backend_depth;}
static void vfs_backend_unlock(void *v) {(void)v;assert(backend_depth==1);--backend_depth;}
static void *page_to_phys(void *p) {return p;}
static void *phys_to_direct(void *p) {assert(vnode_depth==1);return p;}
static int write_page(void *ctx,uint64_t off,void *p,size_t n,uint64_t *written) {
 (void)ctx;assert(backend_depth==1 && off+n<=sizeof(disk));++writes;
 *written=short_write?n-1:n;
 if(fail)return K_EIO;
 memcpy(disk+off,p,*written);return K_OK;
}
''' + lookup + flush + copy_in + r'''
int main(void) {
 uint8_t memory[sizeof(disk)],expected[sizeof(disk)],input[sizeof(disk)];
 memset(input,0x77,sizeof(input));
 vfs_cached_page_t entries[3]={0};vfs_page_cache_t cache={0};
 vfs_backend_node_t backend={.write=write_page};
 vnode_t vnode={&backend,&cache,sizeof(disk)};
 for(unsigned i=0;i<3;++i) {
  entries[i].index=i;entries[i].page=memory+i*PAGE_SIZE;
  vfs_page_cache_insert(&cache,&entries[i]);vfs_page_cache_grow(&cache);
 }
 size_t lengths[]={1,17,PAGE_SIZE-1,PAGE_SIZE,PAGE_SIZE+1,2*PAGE_SIZE};
 unsigned cases=0;
 for(unsigned clean=0;clean<2;++clean) {
  for(size_t off=0;off<=PAGE_SIZE;++off) {
   for(unsigned l=0;l<sizeof(lengths)/sizeof(lengths[0]);++l) {
    size_t n=lengths[l];memset(disk,0x11,sizeof(disk));
    memset(memory,clean?0x11:0x22,sizeof(memory));memcpy(expected,memory,sizeof(memory));
    memset(expected+off,0x77,n);memcpy(disk+off,input,n);
    for(unsigned i=0;i<3;++i)entries[i].dirty=!clean;
    vfs_page_cache_copy_in(&vnode,off,input,n);assert(!vnode_depth);
    assert(!memcmp(memory,expected,sizeof(memory)));
    for(unsigned i=0;i<3;++i) {
     size_t base=i*PAGE_SIZE;
     bool full=off<=base && off+n>=base+PAGE_SIZE;
     assert(entries[i].dirty==(!clean && !full));
     vfs_vnode_lock(&vnode);
     assert(vfs_page_cache_flush_entry(&vnode,&backend,&entries[i])==K_OK);
     vfs_vnode_unlock(&vnode);
    }
    assert(!memcmp(disk,expected,sizeof(disk)));++cases;
   }
  }
 }
 /* Failed and short writeback cannot consume the retained dirty mark. */
 entries[0].dirty=true;vfs_page_cache_copy_in(&vnode,5,input,1);
 vfs_vnode_lock(&vnode);fail=1;
 assert(vfs_page_cache_flush_entry(&vnode,&backend,&entries[0])==K_EIO && entries[0].dirty);
 fail=0;short_write=1;
 assert(vfs_page_cache_flush_entry(&vnode,&backend,&entries[0])==K_EIO && entries[0].dirty);
 short_write=0;
 assert(vfs_page_cache_flush_entry(&vnode,&backend,&entries[0])==K_OK && !entries[0].dirty);
 vfs_vnode_unlock(&vnode);
 /* No destination means no successful full-page replacement. */
 entries[0].page=0;entries[0].dirty=true;
 vfs_page_cache_copy_in(&vnode,0,input,PAGE_SIZE);assert(entries[0].dirty);
 /* Covering the valid EOF prefix is not a full-page replacement. Keeping
  * the mark is conservative; writeback still persists only valid bytes. */
 entries[0].page=memory;vnode.size=17;
 memset(disk,0x11,sizeof(disk));memset(memory,0x22,sizeof(memory));
 memcpy(disk,input,17);vfs_page_cache_copy_in(&vnode,0,input,17);
 assert(entries[0].dirty && vnode.size==17);
 vfs_vnode_lock(&vnode);
 assert(vfs_page_cache_flush_entry(&vnode,&backend,&entries[0])==K_OK);
 vfs_vnode_unlock(&vnode);
 assert(!entries[0].dirty && !memcmp(disk,input,17));
 for(unsigned i=17;i<sizeof(disk);++i)assert(disk[i]==0x11);
 vnode.size=7;entries[1].dirty=true;
 vfs_page_cache_copy_in(&vnode,PAGE_SIZE+3,input,17);
 assert(vnode.size==PAGE_SIZE+20 && entries[1].dirty);
 uint64_t size=vnode.size;vfs_page_cache_copy_in(&vnode,UINT64_MAX-3,input,8);
 assert(vnode.size==size && !vnode_depth && !backend_depth);
 free(cache.expanded_buckets);
 printf("Cache copy-in: %u partial/full/multipage clean+dirty cases, writeback data, failure retry and missing destination passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-copy-in-dirty-') as directory:
    binary = str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x','c','-','-o',binary],input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=60)
