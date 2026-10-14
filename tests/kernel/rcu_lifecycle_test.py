"""Whole production RCU: once publication, reader gate, FIFO and reentry."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/sync/rcu.c').read_text()
code=r'''
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
static atomic_uint init_attempts;
static _Bool parallel;
static _Bool counted_cas(atomic_uint *p,unsigned *expected,unsigned value,
                        memory_order success,memory_order failure) {
    atomic_fetch_add(&init_attempts,1);
    if(parallel)while(atomic_load(&init_attempts)<8)sched_yield();
    return atomic_compare_exchange_strong_explicit(p,expected,value,success,failure);
}
''' + source.replace(
    'atomic_compare_exchange_strong_explicit(&g_rcu.init_state',
    'counted_cas(&g_rcu.init_state') + r'''
static _Thread_local unsigned depth;
void sched_preempt_disable(void) { ++depth; }
void sched_preempt_enable(void) { assert(depth); --depth; }
static void *initializer(void *unused) {
    (void)unused; assert(rcu_init() && !depth);
    assert(g_rcu.head==0 && g_rcu.tail==0 && g_rcu.count==0);
    assert(!atomic_load(&g_rcu.lock.state) && !atomic_load(&g_rcu.gate.state));
    return 0;
}
static unsigned fifo_next;
static void fifo_callback(void *p) {
    assert(!depth && !atomic_load(&g_rcu.lock.state) && !atomic_load(&g_rcu.gate.state));
    assert((unsigned)(uintptr_t)p==fifo_next++);
}
static unsigned reentries;
static void child_callback(void *p) { (void)p; assert(!depth); ++reentries; }
static void parent_callback(void *p) {
    assert(!depth); ++reentries;
    rcu_read_lock(); assert(rcu_read_held()); rcu_read_unlock();
    assert(rcu_call(child_callback,p)==K_OK);
}
static atomic_uint readers_ready;
static atomic_bool release_readers;
static void *reader(void *unused) {
    (void)unused; rcu_read_lock(); rcu_read_lock();
    atomic_fetch_add(&readers_ready,1);
    while(!atomic_load(&release_readers))sched_yield();
    rcu_read_unlock(); rcu_read_unlock(); assert(!depth); return 0;
}
typedef struct item { atomic_uint calls; unsigned id; } item_t;
static item_t items[8000];
static atomic_uint producers_done,completed;
static void item_callback(void *p) {
    item_t *item=p; assert(!depth && item->id<8000);
    assert(atomic_fetch_add(&item->calls,1)==0);
    atomic_fetch_add(&completed,1);
}
static void *producer(void *p) {
    unsigned base=(unsigned)(uintptr_t)p*2000;
    for(unsigned i=base;i<base+2000;++i) {
        kstatus_t status;
        do { status=rcu_call(item_callback,&items[i]); if(status==K_ENOMEM)sched_yield(); }while(status==K_ENOMEM);
        assert(status==K_OK && !depth);
    }
    atomic_fetch_add(&producers_done,1); return 0;
}
int main(void) {
    pthread_t threads[8]; parallel=true;
    g_rcu.head=31; g_rcu.tail=47; g_rcu.count=77;
    atomic_store(&g_rcu.reader_count,59);
    for(unsigned i=0;i<8;++i)assert(!pthread_create(&threads[i],0,initializer,0));
    for(unsigned i=0;i<8;++i)assert(!pthread_join(threads[i],0));
    parallel=false; assert(atomic_load(&init_attempts)==8 && !rcu_read_held());
    assert(rcu_self_test());
    assert(rcu_call(0,0)==K_EINVAL && rcu_poll(0)==0);
    for(unsigned cycle=0;cycle<20;++cycle) {
        unsigned begin=fifo_next;
        rcu_read_lock(); rcu_read_lock();
        assert(atomic_load(&g_rcu.reader_count)==2 && !depth);
        for(unsigned i=0;i<RCU_CALLBACK_CAPACITY;++i)
            assert(rcu_call(fifo_callback,(void *)(uintptr_t)(begin+i))==K_OK);
        assert(rcu_call(fifo_callback,0)==K_ENOMEM && rcu_poll(UINT32_MAX)==0);
        assert(rcu_synchronize()==K_EBUSY && fifo_next==begin);
        rcu_read_unlock(); assert(rcu_poll(1)==0);
        rcu_read_unlock(); assert(!rcu_read_held());
        assert(rcu_poll(17)==17 && fifo_next==begin+17);
        assert(rcu_synchronize()==K_OK && fifo_next==begin+RCU_CALLBACK_CAPACITY);
        assert(!g_rcu.count && !depth);
    }
    unsigned begin=fifo_next;
    for(unsigned i=0;i<RCU_CALLBACK_CAPACITY;++i)
        assert(rcu_call(fifo_callback,(void *)(uintptr_t)(begin+i))==K_OK);
    assert(rcu_poll(UINT32_MAX)==RCU_CALLBACK_CAPACITY);
    assert(fifo_next==begin+RCU_CALLBACK_CAPACITY && rcu_poll(1)==0);
    rcu_read_unlock(); assert(!atomic_load(&g_rcu.reader_count));
    assert(rcu_call(parent_callback,0)==K_OK && rcu_poll(1)==1 && reentries==1);
    assert(rcu_synchronize()==K_OK && reentries==2 && !g_rcu.count);
    for(unsigned i=0;i<6;++i)assert(!pthread_create(&threads[i],0,reader,0));
    while(atomic_load(&readers_ready)<6)sched_yield();
    assert(atomic_load(&g_rcu.reader_count)==12);
    assert(rcu_call(child_callback,0)==K_OK && rcu_poll(1)==0 && rcu_synchronize()==K_EBUSY);
    atomic_store(&release_readers,true);
    for(unsigned i=0;i<6;++i)assert(!pthread_join(threads[i],0));
    assert(rcu_poll(1)==1 && reentries==3 && !rcu_read_held());
    sched_preempt_disable(); sched_preempt_disable();
    rcu_read_lock(); assert(depth==2); rcu_read_unlock(); assert(depth==2);
    sched_preempt_enable(); sched_preempt_enable();
    unsigned attempts=atomic_load(&init_attempts);
    for(unsigned i=0;i<10000;++i) { assert(rcu_init()); rcu_read_lock(); rcu_read_unlock(); }
    assert(atomic_load(&init_attempts)==attempts && !depth);
    for(unsigned i=0;i<8000;++i)items[i].id=i;
    for(unsigned i=0;i<4;++i)assert(!pthread_create(&threads[i],0,producer,(void *)(uintptr_t)i));
    while(atomic_load(&producers_done)<4 || atomic_load(&completed)<8000) {
        (void)rcu_poll(17); sched_yield();
    }
    for(unsigned i=0;i<4;++i)assert(!pthread_join(threads[i],0));
    for(unsigned i=0;i<8000;++i)assert(atomic_load(&items[i].calls)==1);
    assert(rcu_synchronize()==K_OK && !g_rcu.count && !depth);
    assert(atomic_load(&init_attempts)==attempts);
    puts("RCU: eight concurrent initializers, nested/six concurrent readers, 20 full FIFO wraps, callback reentry, 10000 no-CAS reads and 8000 concurrent callbacks exactly once passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-rcu-') as directory:
    binary=str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-x','c','-','-o',binary],input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
