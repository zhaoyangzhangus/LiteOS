"""Actual slab creation from poisoned pages, without a backing-page clear."""
from pathlib import Path
import subprocess
import tempfile
import re

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/mm/kmalloc.c').read_text()
layout=source[source.index('#define KMEM_MAGIC'):source.index('static void bytes_fill(')]
fill=source[source.index('static void bytes_fill('):source.index('static void cache_lock(')]
fill=fill.replace('__builtin_memset(memory, value, size)', 'tracked_fill(memory, value, size)')
headers=source[source.index('static size_t class_capacity('):source.index('static kmem_magazine_t *current_magazine(')]
start=source.index('static slab_page_t *create_slab(')
create=source[start:source.index('/* cache->lock must be held',start)]
previous=create.replace('create_slab(', 'previous_create_slab(').replace(
    'slab->slot_count = g_caches[class_index].slot_count;',
    'slab->slot_count = (PAGE_SIZE-KMEM_SLAB_HEADER_SIZE)/g_slot_sizes[class_index];')
previous=previous.replace('bytes_fill((uint8_t *)payload + sizeof(void *), KMEM_POISON_FREE,\n'
                         '                   capacity - sizeof(void *));',
                         'bytes_fill(payload, KMEM_POISON_FREE, capacity);')
assert previous != create
code=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#define CACHELINE_SIZE 64
#define MAX_CPUS 1
#define PAGE_SIZE 4096
#define PAGE_OWNER_SLAB 7
typedef struct {unsigned owner;} page_t;
typedef struct {atomic_uint state;} spinlock_t;
''' + layout + r'''
static size_t poison_bytes;
static void tracked_fill(void *memory,uint8_t value,size_t size) {
    if(value==KMEM_POISON_FREE)poison_bytes+=size;
    memset(memory,value,size);
}
''' + fill + headers + r'''
static _Alignas(64) uint8_t arena[PAGE_SIZE+128];
static page_t page;
static bool fail_page,fail_direct;
static unsigned frees,requests;
static page_t *page_alloc(unsigned order,unsigned flags) {
    assert(!order && !flags);++requests;return fail_page?NULL:&page;
}
static void page_free(page_t *p) {assert(p==&page);++frees;}
static uintptr_t page_to_phys(page_t *p) {assert(p==&page);return (uintptr_t)(arena+64);}
static void *phys_to_direct(uintptr_t p) {return fail_direct?NULL:(void *)p;}
''' + create + previous + r'''
int main(void) {
    unsigned cases=0;
    for(unsigned cls=0;cls<KMEM_CLASS_COUNT;++cls)
        g_caches[cls].slot_count=(PAGE_SIZE-KMEM_SLAB_HEADER_SIZE)/g_slot_sizes[cls];
    for(unsigned cls=0;cls<KMEM_CLASS_COUNT;++cls)for(unsigned pattern=0;pattern<32;++pattern) {
        uint8_t expected[sizeof(arena)];
        unsigned count=(PAGE_SIZE-KMEM_SLAB_HEADER_SIZE)/g_slot_sizes[cls];
        memset(arena,(int)(0x75+pattern),sizeof(arena));poison_bytes=0;
        assert(previous_create_slab((uint16_t)cls)==(void *)(arena+64));
        size_t old_poison=poison_bytes;
        assert(old_poison==count*class_capacity(cls));
        memcpy(expected,arena,sizeof(arena));poison_bytes=0;
        memset(arena,(int)(0x75+pattern),sizeof(arena));
        slab_page_t *slab=create_slab((uint16_t)cls);assert(slab==(void *)(arena+64));
        assert(!memcmp(expected,arena,sizeof(arena)));
        assert(poison_bytes==old_poison-count*sizeof(void *));
        assert(page.owner==PAGE_OWNER_SLAB && slab->magic==KMEM_SLAB_MAGIC);
        assert(!slab->next && !slab->prev_link && slab->page==&page);
        assert(slab->slot_count==count && slab->free_count==count);
        assert(slab->class_index==cls && !slab->reserved);
        void *link=slab->free_list;
        for(unsigned remaining=count;remaining;--remaining) {
            allocation_header_t *h=(void *)(arena+64+KMEM_SLAB_HEADER_SIZE+
                (remaining-1)*g_slot_sizes[cls]);
            uint8_t *payload=header_payload(h);assert(link==payload);
            assert(payload_header(payload)==h);
            assert(h->magic==KMEM_MAGIC && h->owner==(uintptr_t)slab);
            assert(!h->requested && h->class_index==cls && h->state==KMEM_STATE_FREE);
            assert(!h->flags && !h->cookie);
            assert(bytes_equal((uint8_t *)h+sizeof(*h),KMEM_REDZONE_BYTE,KMEM_FRONT_SIZE));
            assert(bytes_equal(payload+sizeof(void *),KMEM_POISON_FREE,
                               class_capacity(cls)-sizeof(void *)));
            memcpy(&link,payload,sizeof(link));
        }
        assert(!link);
        for(unsigned i=0;i<64;++i)assert(arena[i]==(uint8_t)(0x75+pattern) &&
             arena[PAGE_SIZE+64+i]==(uint8_t)(0x75+pattern));
        ++cases;
    }
    fail_page=true;assert(!create_slab(0) && !frees);fail_page=false;
    fail_direct=true;assert(!create_slab(0) && frees==1);
    assert(requests==2*cases+2);
    printf("Slab creation: %u poisoned-page cases across all classes; full headers/guards/poison/links/canaries and failure cleanup PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-slab-create-') as directory:
    binary=str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-Wno-unused-variable','-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-x','c','-','-o',binary],
                   input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
    assembly_code=code
    assembly_code=assembly_code.replace('static slab_page_t *create_slab(',
        'static __attribute__((used,noinline)) slab_page_t *create_slab(').replace(
        'static slab_page_t *previous_create_slab(',
        'static __attribute__((used,noinline)) slab_page_t *previous_create_slab(')
    assembly=subprocess.run(['cc','-std=c11','-O2','-fno-inline','-S',
                             '-x','c','-','-o','-'],input=assembly_code,
                            text=True,capture_output=True,check=True).stdout
    def body(name):
        return assembly.split(name+':\n',1)[1].split('\t.size',1)[0]
    divide=r'\b(?:idiv|div)[bwlq]?\b'
    assert re.search(divide,body('previous_create_slab'))
    assert not re.search(divide,body('create_slab'))
    print('Slab creation assembly: old per-page division present; cached production path has no division')
    print('Slab poison: exact old/new whole-page comparison; 8 overwritten bytes per slot no longer filled (up to 336 bytes per page)')
