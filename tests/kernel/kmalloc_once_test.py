"""Actual allocator initialization publication and initialized fast path."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/mm/kmalloc.c').read_text()
layout=source[source.index('#define KMEM_MAGIC'):source.index('static void bytes_fill(')]
start=source.index('static void initialize_allocator(')
initializer=source[start:source.index('static uint16_t class_index_for_size(',start)]
initializer=initializer.replace('atomic_compare_exchange_strong_explicit(&g_init_state',
                                'counted_cas(&g_init_state')
code=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>
#include <stdio.h>
#define CACHELINE_SIZE 64
#define MAX_CPUS 8
#define PAGE_SIZE 4096U
typedef struct { atomic_uint state; } spinlock_t;
typedef struct { unsigned owner; } page_t;
''' + layout + r'''
static atomic_uint attempts,disables,enables;
static _Thread_local unsigned depth;
static bool parallel=true;
static void sched_preempt_disable(void) { ++depth;atomic_fetch_add(&disables,1); }
static void sched_preempt_enable(void) { assert(depth);--depth;atomic_fetch_add(&enables,1); }
static bool counted_cas(atomic_uint *p,unsigned *expected,unsigned value,
                        memory_order success,memory_order failure) {
    assert(depth);atomic_fetch_add(&attempts,1);
    if(parallel)while(atomic_load(&attempts)<8)sched_yield();
    return atomic_compare_exchange_strong_explicit(p,expected,value,success,failure);
}
''' + initializer + r'''
static void verify(void) {
    assert(!atomic_load(&g_error_count));
    for(unsigned i=0;i<KMEM_CLASS_COUNT;++i) {
        assert(!atomic_load(&g_caches[i].lock.state) && !g_caches[i].slabs);
        assert(g_caches[i].slot_size==g_slot_sizes[i]);
        assert(g_caches[i].slot_count==(PAGE_SIZE-KMEM_SLAB_HEADER_SIZE)/g_slot_sizes[i]);
        assert(g_cache_owner[i]==UINT32_MAX);
    }
    for(unsigned cpu=0;cpu<MAX_CPUS;++cpu) {
        assert(!atomic_load(&g_cpu_stats[cpu].fastpath_hits));
        assert(!atomic_load(&g_cpu_stats[cpu].fastpath_refills));
        for(unsigned i=0;i<KMEM_CLASS_COUNT;++i) {
            const kmem_magazine_t *m=&g_magazines[cpu].classes[i];
            assert(!m->head && !m->count && !m->reserved);
        }
    }
}
static void *initialize(void *unused) {
    (void)unused;initialize_allocator();assert(!depth);verify();return NULL;
}
int main(void) {
    memset(g_caches,0x75,sizeof(g_caches));memset(g_magazines,0x75,sizeof(g_magazines));
    memset(g_cpu_stats,0x75,sizeof(g_cpu_stats));atomic_store(&g_error_count,77);
    pthread_t threads[8];
    for(unsigned i=0;i<8;++i)assert(!pthread_create(&threads[i],NULL,initialize,NULL));
    for(unsigned i=0;i<8;++i)assert(!pthread_join(threads[i],NULL));
    assert(atomic_load(&attempts)==8 && atomic_load(&disables)==8 && atomic_load(&enables)==8);
    parallel=false;
    g_magazines[0].classes[0].head=(void *)(uintptr_t)0x1000;
    g_magazines[0].classes[0].count=7;
    atomic_store(&g_cpu_stats[0].fastpath_hits,123);
    for(unsigned i=0;i<100000;++i) {
        depth=i%3;unsigned before=depth;initialize_allocator();assert(depth==before);
    }
    assert(atomic_load(&attempts)==8 && atomic_load(&disables)==8 && atomic_load(&enables)==8);
    assert(g_magazines[0].classes[0].head==(void *)(uintptr_t)0x1000);
    assert(g_magazines[0].classes[0].count==7 && atomic_load(&g_cpu_stats[0].fastpath_hits)==123);
    for(unsigned i=0;i<KMEM_CLASS_COUNT;++i)
        assert(g_caches[i].slot_count==(PAGE_SIZE-KMEM_SLAB_HEADER_SIZE)/g_slot_sizes[i]);
    puts("Allocator once: eight concurrent initializers, complete publication and 100000 fast calls without CAS/preemption or cache reset PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-kmem-once-') as directory:
    binary=str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-Wno-unused-variable','-pthread','-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-x','c','-','-o',binary],
                   input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
