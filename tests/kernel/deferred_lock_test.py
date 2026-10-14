"""Actual deferred locks/init: publication, IRQ guards and contention traffic."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/irq/deferred.c').read_text()
layout = source[source.index('#define DEFERRED_QUEUE_CAPACITY'):source.index('static void deferred_debug_set(')]
locks = source[source.index('static uint64_t deferred_lock('):source.index('static void deferred_wake_worker(')]
initialize = source[source.index('bool deferred_init(void) {'):source.index('bool deferred_start_worker(void) {')]
check = '    if (atomic_load_explicit(&g_deferred.init_state, memory_order_acquire) == 2U) return true;\n'
assert initialize.count(check) == 1
previous = initialize.replace('deferred_init(', 'previous_init(').replace(check, '')
program = r'''
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
static atomic_uint attempts,polls,waiting,init_claims,ready;
static atomic_bool go;
static _Thread_local bool irq_enabled;
static _Thread_local unsigned depth,initial_depth,cpu,saves,restores,reads;
static bool test_acquire(atomic_uint *state,unsigned *expected,unsigned desired,
                         memory_order success,memory_order failure) {
    assert(!irq_enabled&&depth==initial_depth+1);
    atomic_fetch_add_explicit(&attempts,1,memory_order_relaxed);
    return atomic_compare_exchange_strong_explicit(state,expected,desired,success,failure);
}
static unsigned test_load(atomic_uint *state,memory_order order) {
    assert(!irq_enabled&&depth==initial_depth+1);
    atomic_fetch_add_explicit(&polls,1,memory_order_relaxed);
    return atomic_load_explicit(state,order);
}
#pragma push_macro("atomic_compare_exchange_strong_explicit")
#pragma push_macro("atomic_load_explicit")
#undef atomic_compare_exchange_strong_explicit
#undef atomic_load_explicit
#define atomic_compare_exchange_strong_explicit test_acquire
#define atomic_load_explicit test_load
#include <kernel/spinlock.h>
#pragma pop_macro("atomic_load_explicit")
#pragma pop_macro("atomic_compare_exchange_strong_explicit")
typedef void (*deferred_work_fn_t)(void *);
typedef struct {atomic_uint state;} thread_t;
''' + layout + r'''
static uint64_t deferred_irq_save(void) {
    assert(depth==initial_depth);uint64_t flags=irq_enabled?1ULL<<9:0;
    irq_enabled=false;++saves;return flags;
}
static void deferred_irq_restore(uint64_t flags) {
    assert(!irq_enabled&&depth==initial_depth);++restores;
    irq_enabled=(flags&(1ULL<<9))!=0;
}
static uint32_t x86_current_cpu_index(void) {assert(!irq_enabled);++reads;return cpu;}
static void sched_preempt_disable(void) {assert(!irq_enabled);++depth;}
static void sched_preempt_enable(void) {assert(!irq_enabled&&depth==initial_depth+1);--depth;}
static bool test_init_claim(atomic_uint *state,unsigned *expected,unsigned desired,
                            memory_order success,memory_order failure) {
    atomic_fetch_add_explicit(&init_claims,1,memory_order_relaxed);
    bool won=atomic_compare_exchange_strong_explicit(state,expected,desired,success,failure);
    if(won)while(atomic_load_explicit(&init_claims,memory_order_acquire)<4) {
        __asm__ volatile ("pause");
    }
    return won;
}
#undef atomic_compare_exchange_strong_explicit
#define atomic_compare_exchange_strong_explicit test_init_claim
''' + locks + initialize + previous + r'''
static void *initializer(void *unused) {
    (void)unused;atomic_fetch_add_explicit(&ready,1,memory_order_release);
    while(!atomic_load_explicit(&go,memory_order_acquire)) {__asm__ volatile ("pause");}
    assert(deferred_init());
    assert(g_deferred.head==0&&g_deferred.tail==0&&g_deferred.count==0);
    assert(!g_deferred.critical_burst&&!g_deferred.critical_item.function);
    assert(!g_deferred.critical_ready);
    assert(!atomic_load(&g_deferred.worker_started));return 0;
}
static uint64_t value,checksum;
static void *worker(void *argument) {
    cpu=(unsigned)(uintptr_t)argument;irq_enabled=true;depth=initial_depth=2;
    atomic_fetch_add(&waiting,1);
    for(unsigned n=0;n<20000;++n) {
        uint64_t flags=deferred_lock();
        assert(!irq_enabled&&depth==3&&flags==(1ULL<<9));
        assert(__atomic_load_n(&g_deferred_lock_owner,__ATOMIC_ACQUIRE)==cpu);
        assert(!__atomic_load_n(&g_deferred_waiting[cpu],__ATOMIC_ACQUIRE));
        assert(checksum==value*17);++value;checksum=value*17;
        deferred_unlock(flags);assert(irq_enabled&&depth==2);
    }
    assert(saves==20000&&restores==saves&&reads==saves);return 0;
}
int main(void) {
    pthread_t threads[4];
    g_deferred.count=91;g_deferred.head=19;g_deferred.tail=110;
    for(unsigned n=0;n<4;++n)assert(!pthread_create(&threads[n],0,initializer,0));
    while(atomic_load_explicit(&ready,memory_order_acquire)!=4) {__asm__ volatile ("pause");}
    atomic_store_explicit(&go,true,memory_order_release);
    for(unsigned n=0;n<4;++n)assert(!pthread_join(threads[n],0));
    assert(atomic_load(&init_claims)==4&&atomic_load(&g_deferred.init_state)==2);
    g_deferred.count=44;g_deferred.head=7;g_deferred.tail=51;g_deferred.critical_burst=3;
    atomic_store(&g_deferred.worker_started,true);
    g_deferred.critical_ready=true;
    unsigned char snapshot[sizeof(g_deferred)];memcpy(snapshot,&g_deferred,sizeof(snapshot));
    unsigned before=atomic_load(&init_claims);
    for(unsigned n=0;n<10000;++n)assert(previous_init());
    assert(atomic_load(&init_claims)==before+10000);
    before=atomic_load(&init_claims);
    for(unsigned n=0;n<10000;++n)assert(deferred_init());
    assert(atomic_load(&init_claims)==before&&!memcmp(snapshot,&g_deferred,sizeof(snapshot)));
    unsigned cases=0;
    for(unsigned index=0;index<MAX_CPUS+2;++index)
    for(unsigned state=0;state<2;++state)
    for(initial_depth=0;initial_depth<4;++initial_depth) {
        cpu=index==MAX_CPUS+1?UINT32_MAX:index;irq_enabled=state;depth=initial_depth;
        saves=restores=reads=0;
        memset((void *)g_deferred_waiting,0x77,sizeof(g_deferred_waiting));
        uint64_t flags=deferred_lock();
        assert(!irq_enabled&&depth==initial_depth+1&&reads==1&&saves==1);
        assert(flags==(state?1ULL<<9:0));
        assert(__atomic_load_n(&g_deferred_lock_owner,__ATOMIC_ACQUIRE)==cpu);
        for(unsigned n=0;n<MAX_CPUS;++n)
            assert(g_deferred_waiting[n]==(n==cpu?0:0x77777777U));
        deferred_unlock(flags);
        assert(depth==initial_depth&&irq_enabled==(bool)state&&restores==1);
        assert(g_deferred_lock_owner==UINT32_MAX&&!atomic_load(&g_deferred.lock.state));
        ++cases;
    }
    cpu=MAX_CPUS-1;depth=initial_depth=0;irq_enabled=true;
    memset((void *)g_deferred_waiting,0,sizeof(g_deferred_waiting));
    atomic_store(&attempts,0);atomic_store(&polls,0);
    uint64_t flags=deferred_lock();
    for(unsigned n=0;n<4;++n)assert(!pthread_create(&threads[n],0,worker,(void *)(uintptr_t)n));
    while(atomic_load(&waiting)!=4||atomic_load(&attempts)<5||atomic_load(&polls)<20000)sched_yield();
    assert(atomic_load(&attempts)==5); /* holder plus four failed contenders */
    deferred_unlock(flags);
    for(unsigned n=0;n<4;++n)assert(!pthread_join(threads[n],0));
    assert(value==80000&&checksum==1360000&&!atomic_load(&g_deferred.lock.state));
    assert(!memcmp(snapshot,&g_deferred,sizeof(snapshot)));
    assert(atomic_load(&init_claims)==before);
    printf("Deferred: %u exact CPU/IRQ/nested-preemption guard cases; 80000 contended protected updates; 20000+ held-lock reads with only four failed acquire writes; four-way initial publication; 10000 initialized CAS attempts -> 0; live queue/worker state preserved\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-deferred-lock-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-variable', '-pthread', '-fsanitize=address,undefined',
                    '-fno-pie', '-no-pie', '-I' + str(root / 'include'),
                    '-x', 'c', '-', '-o', binary], input=program, text=True, check=True)
    subprocess.run([binary], check=True, timeout=60)
