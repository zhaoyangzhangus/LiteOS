"""Production small-object free versus full-poison oracle, including flush."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/mm/kmalloc.c').read_text()
layout = source[source.index('#define KMEM_MAGIC'):source.index('static void bytes_fill(')]
fill = source[source.index('static void bytes_fill('):source.index('static void cache_lock(')]
fill = fill.replace('__builtin_memset(memory, value, size)', 'tracked_fill(memory, value, size)')
accessors = source[source.index('static size_t class_capacity('):source.index('static void record_fastpath_hit(')]
guards = source[source.index('static uint64_t allocation_cookie('):source.index('static slab_page_t *create_slab(')]
lists = source[source.index('static slab_page_t *cache_partial_head('):source.index('/* Caller holds cache->lock;')]
flush = source[source.index('static void magazine_flush('):source.index('static void *activate_payload(')]
free = source[source.index('static void kfree_internal('):source.index('void kfree(')]
old = free.replace('kfree_internal(', 'previous_free(').replace(
    'bytes_fill((uint8_t *)pointer + sizeof(void *), KMEM_POISON_FREE,\n'
    '               capacity - sizeof(void *));',
    'bytes_fill(pointer, KMEM_POISON_FREE, capacity);')
assert old != free
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#define CACHELINE_SIZE 64
#define MAX_CPUS 1
#define PAGE_OWNER_SLAB 7
typedef struct {unsigned owner;} page_t;
typedef struct {atomic_uint state;} spinlock_t;
''' + layout + r'''
static size_t poisoned;static unsigned errors,frees,locks,depth,cpu;
static void tracked_fill(void *p,uint8_t v,size_t n){
 if(v==KMEM_POISON_FREE)poisoned+=n;memset(p,v,n);
}
static void record_error(void){++errors;}
static void page_free(page_t *p){assert(p&&p->owner==PAGE_OWNER_SLAB);++frees;}
static void cache_lock(kmem_cache_t *c){assert(c&&!depth);++depth;++locks;}
static void cache_unlock(kmem_cache_t *c){assert(c&&depth==1);--depth;}
static unsigned x86_current_cpu_index(void){return cpu;}
''' + fill + accessors + guards + lists + flush + free + old + r'''
static _Alignas(64) uint8_t arena[65536];
static page_t page;
static void *setup(unsigned cls,unsigned request,unsigned count,unsigned corrupt){
 memset(arena,0x71,sizeof(arena));memset(g_caches,0,sizeof(g_caches));
 memset(g_magazines,0,sizeof(g_magazines));
 errors=frees=locks=depth=0;poisoned=0;page.owner=PAGE_OWNER_SLAB;
 slab_page_t *s=(void*)(arena+64);memset(s,0,sizeof(*s));
 s->magic=KMEM_SLAB_MAGIC;s->page=&page;s->class_index=cls;s->slot_count=32;
 for(unsigned i=0;i<=count;++i){
  allocation_header_t *h=(void*)(arena+128+i*g_slot_sizes[cls]);
  memset(h,0,sizeof(*h));h->magic=KMEM_MAGIC;h->owner=(uintptr_t)s;
  h->class_index=cls;h->state=i?KMEM_STATE_FREE:KMEM_STATE_ALLOCATED;
  h->requested=i?0:request;h->flags=0x12;
  uint8_t *p=header_payload(h);
  memset((uint8_t*)h+sizeof(*h),KMEM_REDZONE_BYTE,KMEM_FRONT_SIZE);
  if(i){
   memset(p,KMEM_POISON_FREE,class_capacity(cls));
   *(void**)p=g_magazines[0].classes[cls].head;
   g_magazines[0].classes[cls].head=p;++g_magazines[0].classes[cls].count;
  }else{
   h->cookie=allocation_cookie(h,p);write_trailer(p,request,h->cookie);
   if(corrupt==1)((uint8_t*)h)[sizeof(*h)]^=1;
   if(corrupt==2)p[request]^=1;
  }
 }
 return arena+128+KMEM_PAYLOAD_OFFSET;
}
int main(void){
 unsigned cases=0;
 for(unsigned cls=0;cls<KMEM_CLASS_COUNT;++cls)
 for(unsigned r=0;r<6;++r)for(unsigned route=0;route<2;++route)
 for(unsigned c=0;c<3;++c)for(unsigned corrupt=0;corrupt<3;++corrupt){
  unsigned capacity=class_capacity(cls);
  unsigned lengths[]={0,1,7,8,capacity-1,capacity};unsigned request=lengths[r];
  unsigned counts[]={0,8,16};cpu=route?MAX_CPUS:0;
  void *p=setup(cls,request,counts[c],corrupt);
  previous_free(p);assert(!depth);size_t old_bytes=poisoned;
  uint8_t expected[sizeof(arena)];kmem_cache_t caches[KMEM_CLASS_COUNT];
  kmem_cpu_magazines_t mags[MAX_CPUS];
  memcpy(expected,arena,sizeof(arena));memcpy(caches,g_caches,sizeof(caches));
  memcpy(mags,g_magazines,sizeof(mags));
  unsigned e=errors,f=frees,l=locks;
  p=setup(cls,request,counts[c],corrupt);kfree_internal(p);
  assert(!depth&&!memcmp(expected,arena,sizeof(arena)));
  assert(!memcmp(caches,g_caches,sizeof(caches))&&!memcmp(mags,g_magazines,sizeof(mags)));
  assert(errors==e&&frees==f&&locks==l&&poisoned+sizeof(void*)==old_bytes);
  assert(errors==(corrupt!=0));
  size_t bytes=poisoned;unsigned prior=errors;
  kfree_internal(p);assert(errors==prior+1&&poisoned==bytes);
  ++cases;
 }
 /* Invalid input must not poison or publish any slot. */
 for(unsigned bad=0;bad<5;++bad){
  void *p=setup(0,1,0,0);allocation_header_t *h=payload_header(p);
  slab_page_t *s=(void*)h->owner;
  if(bad==0)h->magic=0;if(bad==1)h->class_index=KMEM_CLASS_COUNT;
  if(bad==2)h->owner=0;if(bad==3)s->magic=0;if(bad==4)s->class_index=1;
  kfree_internal(p);assert(errors==1&&!poisoned&&!frees&&!locks);
 }
 kfree_internal(0);
 /* Large objects retain their full requested-byte poison and page release. */
 for(unsigned n=0;n<=4096;n+=128){
  void *p=setup(9,0,0,0);allocation_header_t *h=payload_header(p);
  h->class_index=KMEM_CLASS_LARGE;h->owner=(uintptr_t)&page;h->requested=n;
  h->cookie=allocation_cookie(h,p);write_trailer(p,n,h->cookie);
  kfree_internal(p);assert(!errors&&frees==1&&poisoned==n&&!locks);
 }
 printf("Small free: %u exact old/new memory, guards, magazine/flush/cache states; 8 fewer poison bytes per release; invalid/double-free and large paths preserved\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-slab-free-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-variable', '-Wno-unused-function',
                    '-Wno-misleading-indentation', '-fsanitize=address,undefined',
                    '-fno-pie', '-no-pie', '-x', 'c', '-', '-o', binary],
                   input=code, text=True, check=True)
    subprocess.run([binary], check=True)
