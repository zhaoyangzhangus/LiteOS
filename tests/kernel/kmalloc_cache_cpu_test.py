"""Actual cache lock helpers: migration before pin and nested preemption."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/mm/kmalloc.c').read_text()
helpers = source[source.index('static void cache_lock('):
                 source.index('uint32_t kmem_debug_cache_waiting(')]
old = helpers.replace('cache_lock(', 'previous_lock(').replace(
    'cache_unlock(', 'previous_unlock(')
old = old.replace('    sched_preempt_disable();\n', '', 1).replace(
    '    spinlock_lock(&cache->lock);',
    '    sched_preempt_disable();\n    spinlock_lock(&cache->lock);', 1)
assert old != helpers
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define MAX_CPUS 256U
#define KMEM_CLASS_COUNT 10U
typedef struct { unsigned state; } spinlock_t;
typedef struct { spinlock_t lock; } kmem_cache_t;
static kmem_cache_t g_caches[KMEM_CLASS_COUNT];
static volatile uint32_t g_cache_owner[KMEM_CLASS_COUNT];
static volatile uint32_t g_cache_waiting[MAX_CPUS],g_cache_class[MAX_CPUS];
static unsigned cpu,depth,next_cpu,pins,unpins,locks,unlocks;
static uint32_t x86_current_cpu_index(void) { return cpu; }
static void sched_preempt_disable(void) {
    ++pins;if(depth==0)cpu=next_cpu;++depth;
}
static void sched_preempt_enable(void) { assert(depth);++unpins;--depth; }
static void spinlock_lock(spinlock_t *lock) {
    assert(depth&&!lock->state);lock->state=1;++locks;
}
static void spinlock_unlock(spinlock_t *lock) {
    unsigned index=0;
    while(index<KMEM_CLASS_COUNT&&lock!=&g_caches[index].lock)++index;
    assert(index<KMEM_CLASS_COUNT&&depth&&lock->state);
    assert(g_cache_owner[index]==UINT32_MAX);
    lock->state=0;++unlocks;
}
''' + helpers + old + r'''
static void reset(unsigned start,unsigned nested) {
    cpu=start;next_cpu=(start+1)%MAX_CPUS;depth=nested;
    pins=unpins=locks=unlocks=0;
    for(unsigned i=0;i<MAX_CPUS;++i) {
        g_cache_waiting[i]=7;g_cache_class[i]=UINT32_MAX;
    }
    for(unsigned i=0;i<KMEM_CLASS_COUNT;++i) {
        g_caches[i].lock.state=0;g_cache_owner[i]=UINT32_MAX;
    }
}
int main(void) {
    unsigned cases=0,old_misreports=0;
    for(unsigned start=0;start<MAX_CPUS;++start)
    for(unsigned nested=0;nested<4;++nested)
    for(unsigned index=0;index<KMEM_CLASS_COUNT;++index) {
        unsigned owner=nested?start:(start+1)%MAX_CPUS;
        reset(start,nested);previous_lock(&g_caches[index]);
        assert(g_cache_owner[index]==start);
        old_misreports+=g_cache_owner[index]!=cpu;
        previous_unlock(&g_caches[index]);assert(depth==nested);
        reset(start,nested);cache_lock(&g_caches[index]);
        assert(cpu==owner&&g_cache_owner[index]==owner);
        assert(g_cache_waiting[owner]==0&&g_cache_class[owner]==index);
        for(unsigned i=0;i<MAX_CPUS;++i)if(i!=owner) {
            assert(g_cache_waiting[i]==7&&g_cache_class[i]==UINT32_MAX);
        }
        assert(depth==nested+1&&pins==1&&locks==1);
        cache_unlock(&g_caches[index]);
        assert(depth==nested&&unpins==1&&unlocks==1);
        assert(!g_caches[index].lock.state&&g_cache_owner[index]==UINT32_MAX);
        ++cases;
    }
    assert(cases==10240&&old_misreports==2560);
    printf("Cache CPU diagnostics: %u migration/nesting/class cases; "
           "%u old wrong-owner reports corrected; lock/preemption balanced\n",
           cases,old_misreports);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-cache-cpu-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(path / 'test.c'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
