"""Actual VM/vmalloc once initialization, publication and cache preservation."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
vm=(root/'kernel/mm/vm_space.c').read_text()
malloc=(root/'kernel/mm/vmalloc.c').read_text()
def function(source,name):
    start=source.index('static void '+name+'(')
    return source[start:source.index('\n}',start)+2]
layout=malloc[malloc.index('#define VMALLOC_MANAGED_SIZE'):malloc.index('static uint64_t vmalloc_lock_irqsave(')]
functions=function(vm,'vm_global_initialize')+'\n'+function(malloc,'vmalloc_initialize')
functions=functions.replace('atomic_compare_exchange_strong_explicit(', 'counted_cas(')
code=r'''
#include <kernel/mm.h>
#include <kernel/spinlock.h>
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>
#include <stdio.h>
static atomic_uint_fast32_t g_next_pcid;
static atomic_uint g_vm_init_state;
static atomic_uint attempts;
static bool parallel=true;
''' + layout + r'''
static bool counted_cas(atomic_uint *p,unsigned *expected,unsigned value,
                        memory_order success,memory_order failure) {
    atomic_fetch_add(&attempts,1);
    if(parallel)while(atomic_load(&attempts)<8)sched_yield();
    return atomic_compare_exchange_strong_explicit(p,expected,value,success,failure);
}
''' + functions + r'''
static unsigned target;
static void *initialize(void *unused) {
    (void)unused;
    if(target==0) {
        vm_global_initialize();assert(atomic_load(&g_next_pcid)==1);
    } else {
        vmalloc_initialize();assert(!atomic_load(&g_vmalloc_lock.state));
        assert(!g_vmalloc_next_free_area);
        assert(!g_vmalloc_area_limit);
        for(unsigned i=0;i<VMALLOC_BITMAP_WORDS;++i)assert(!g_vmalloc_bitmap[i]);
        for(unsigned i=0;i<VMALLOC_MAX_AREAS;++i)assert(!g_vmalloc_areas[i].used);
    }
    return NULL;
}
int main(void) {
    for(target=0;target<2;++target) {
        atomic_store(&attempts,0);parallel=true;
        atomic_store(&g_next_pcid,77);
        memset(g_vmalloc_bitmap,0x75,sizeof(g_vmalloc_bitmap));
        for(unsigned i=0;i<VMALLOC_MAX_AREAS;++i)g_vmalloc_areas[i].used=true;
        atomic_store(&g_vmalloc_lock.state,17);
        g_vmalloc_next_free_area=731;
        g_vmalloc_area_limit=819;
        pthread_t threads[8];
        for(unsigned i=0;i<8;++i)assert(!pthread_create(&threads[i],NULL,initialize,NULL));
        for(unsigned i=0;i<8;++i)assert(!pthread_join(threads[i],NULL));
        assert(atomic_load(&attempts)==8);parallel=false;
        atomic_store(&g_next_pcid,123);
        g_vmalloc_bitmap[0]=UINT64_MAX;g_vmalloc_areas[0].used=true;
        atomic_store(&g_vmalloc_lock.state,19);
        g_vmalloc_next_free_area=517;
        g_vmalloc_area_limit=911;
        for(unsigned i=0;i<100000;++i)
            if(target==0)vm_global_initialize();else vmalloc_initialize();
        assert(atomic_load(&attempts)==8 && atomic_load(&g_next_pcid)==123);
        assert(g_vmalloc_bitmap[0]==UINT64_MAX && g_vmalloc_areas[0].used);
        assert(atomic_load(&g_vmalloc_lock.state)==19);
        assert(g_vmalloc_next_free_area==517);
        assert(g_vmalloc_area_limit==911);
    }
    puts("VM/vmalloc once: eight concurrent initializers each, published PCID/bitmap/areas/lock and 200000 fast calls without CAS/state reset PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-vm-once-') as directory:
    binary=str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-Wno-unused-variable','-pthread','-I'+str(root/'include'),
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x','c','-','-o',binary],input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
