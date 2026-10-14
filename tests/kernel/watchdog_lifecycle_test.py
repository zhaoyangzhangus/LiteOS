"""Production watchdog with controlled time and a bounded deferred queue."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/watchdog.c').read_text()
code=r'''
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
static atomic_uint attempts;
static _Thread_local unsigned preempt_depth;
void sched_preempt_disable(void) {++preempt_depth;}
void sched_preempt_enable(void) {assert(preempt_depth);--preempt_depth;}
static _Bool parallel;
static _Bool counted_cas(atomic_uint *p,unsigned *expected,unsigned value,
                        memory_order success,memory_order failure) {
    atomic_fetch_add(&attempts,1);
    if(parallel)while(atomic_load(&attempts)<8)sched_yield();
    return atomic_compare_exchange_strong_explicit(p,expected,value,success,failure);
}
''' + source.replace('atomic_compare_exchange_strong_explicit(&g_watchdog.init_state',
                     'counted_cas(&g_watchdog.init_state') + r'''
static atomic_uint_fast64_t now=100;
uint64_t x86_read_tsc(void) { return atomic_load(&now); }
uint64_t x86_timeout_ns_to_tsc(uint64_t ns) { return ns; }
typedef struct work { deferred_work_fn_t fn; void *arg; } work_t;
static work_t queue[WATCHDOG_MAX_CLIENTS];
static unsigned queued,schedule_attempts;
static bool queue_full;
bool deferred_try_schedule(deferred_work_fn_t fn,void *arg) {
    assert(atomic_load(&g_watchdog.lock.state) && preempt_depth); ++schedule_attempts;
    if(queue_full || queued==WATCHDOG_MAX_CLIENTS)return false;
    queue[queued++]=(work_t){fn,arg}; return true;
}
uint32_t deferred_run(uint32_t budget) {
    assert(!atomic_load(&g_watchdog.lock.state) && !preempt_depth); unsigned count=0;
    while(count<budget && queued) {
        work_t w=queue[0]; memmove(queue,queue+1,(--queued)*sizeof(work_t));
        w.fn(w.arg); ++count;
    }
    return count;
}
typedef struct context { watchdog_client_t *client; unsigned calls; bool stop; } context_t;
static void expired(void *p) {
    context_t *c=p; assert(!atomic_load(&g_watchdog.lock.state) && !preempt_depth);
    assert(watchdog_get_state(c->client)==WATCHDOG_EXPIRED);
    ++c->calls;
    if(c->stop)assert(watchdog_stop(c->client)==K_OK);
}
static void *initializer(void *unused) {
    (void)unused; assert(watchdog_manager_init());
    assert(!atomic_load(&g_watchdog.lock.state));
    for(unsigned i=0;i<WATCHDOG_MAX_CLIENTS;++i) {
        assert(!g_watchdog.clients[i].registered && !g_watchdog.clients[i].queued && !g_watchdog.clients[i].notified);
        assert(atomic_load(&g_watchdog.clients[i].state)==WATCHDOG_STOPPED);
    }
    return 0;
}
static void *registrar(void *unused) {
    (void)unused;
    for(unsigned i=0;i<1000;++i) {
        watchdog_client_t *c=0;
        assert(watchdog_register("parallel",10,expired,0,&c)==K_OK);
        assert(watchdog_start(c)==K_OK && watchdog_kick(c)==K_OK);
        assert(watchdog_stop(c)==K_OK && watchdog_unregister(c)==K_OK);
        assert(!preempt_depth);
    }
    return 0;
}
int main(void) {
    assert(watchdog_poll(UINT64_MAX)==0); /* IRQ poll never initializes. */
    pthread_t threads[8]; parallel=true;
    for(unsigned i=0;i<WATCHDOG_MAX_CLIENTS;++i) {
        g_watchdog.clients[i].registered=true; g_watchdog.clients[i].queued=true;
        g_watchdog.clients[i].notified=true; atomic_store(&g_watchdog.clients[i].state,WATCHDOG_EXPIRED);
    }
    for(unsigned i=0;i<8;++i)assert(!pthread_create(&threads[i],0,initializer,0));
    for(unsigned i=0;i<8;++i)assert(!pthread_join(threads[i],0));
    parallel=false; assert(atomic_load(&attempts)==8 && watchdog_self_test());
    context_t contexts[WATCHDOG_MAX_CLIENTS]={0};
    for(unsigned i=0;i<WATCHDOG_MAX_CLIENTS;++i)
        assert(watchdog_register("client",10,expired,&contexts[i],&contexts[i].client)==K_OK);
    watchdog_client_t *sentinel=(void *)(uintptr_t)1;
    assert(watchdog_register("full",10,expired,0,&sentinel)==K_ENOMEM && sentinel==(void *)(uintptr_t)1);
    uintptr_t base=(uintptr_t)g_watchdog.clients;
    for(unsigned offset=0;offset<sizeof(g_watchdog.clients);++offset) {
        watchdog_client_t *p=(void *)(base+offset); bool valid=offset%sizeof(*p)==0;
        assert(watchdog_known_locked(p)==valid);
        assert(watchdog_get_state(p)==(valid?WATCHDOG_STOPPED:WATCHDOG_EXPIRED));
        if(!valid)assert(watchdog_start(p)==K_ENOENT && watchdog_stop(p)==K_ENOENT && watchdog_unregister(p)==K_ENOENT);
    }
    const uintptr_t bad[]={0,1,base-1,base+sizeof(g_watchdog.clients),UINTPTR_MAX};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
        assert(!watchdog_known_locked((void *)bad[i]));
        assert(watchdog_kick((void *)bad[i])==K_ENOENT && watchdog_expiration_count((void *)bad[i])==0);
    }
    for(unsigned i=0;i<WATCHDOG_MAX_CLIENTS;++i) {
        assert(watchdog_kick(contexts[i].client)==K_EBUSY);
        assert(watchdog_start(contexts[i].client)==K_OK && watchdog_start(contexts[i].client)==K_EBUSY);
    }
    unsigned before=schedule_attempts;
    watchdog_lock(); assert(watchdog_poll(110)==0 && schedule_attempts==before);
    assert(atomic_load(&g_watchdog.lock.state)==1); watchdog_unlock();
    assert(watchdog_poll(109)==0);
    queue_full=true;
    assert(watchdog_poll(110)==WATCHDOG_MAX_CLIENTS && !queued);
    assert(watchdog_poll(110)==0 && schedule_attempts==before+2*WATCHDOG_MAX_CLIENTS);
    queue_full=false;
    assert(watchdog_poll(110)==0 && queued==WATCHDOG_MAX_CLIENTS);
    before=schedule_attempts; assert(watchdog_poll(110)==0 && schedule_attempts==before);
    for(unsigned i=0;i<WATCHDOG_MAX_CLIENTS;i+=2)assert(watchdog_stop(contexts[i].client)==K_OK);
    assert(deferred_run(UINT32_MAX)==WATCHDOG_MAX_CLIENTS && !queued);
    for(unsigned i=0;i<WATCHDOG_MAX_CLIENTS;++i) {
        assert(contexts[i].calls==(i%2) && watchdog_expiration_count(contexts[i].client)==1);
        assert(watchdog_stop(contexts[i].client)==K_OK && watchdog_unregister(contexts[i].client)==K_OK);
    }
    context_t c={.stop=true}; assert(watchdog_register("repeat",10,expired,&c,&c.client)==K_OK);
    for(unsigned cycle=0;cycle<1000;++cycle) {
        atomic_store(&now,1000+cycle*30);
        assert(watchdog_start(c.client)==K_OK);
        uint64_t old=c.client->deadline_tsc;
        atomic_fetch_add(&now,1); assert(watchdog_kick(c.client)==K_OK);
        assert(c.client->deadline_tsc==old+1 && watchdog_poll(old)==0);
        assert(watchdog_poll(old+1)==1 && queued==1 && watchdog_poll(old+1)==0);
        assert(deferred_run(1)==1 && c.calls==cycle+1 && watchdog_get_state(c.client)==WATCHDOG_STOPPED);
    }
    assert(watchdog_expiration_count(c.client)==1000 && watchdog_unregister(c.client)==K_OK);
    assert(watchdog_register(0,10,expired,0,&sentinel)==K_EINVAL);
    assert(watchdog_register("x",0,expired,0,&sentinel)==K_EINVAL);
    assert(watchdog_register("x",UINT64_MAX,expired,0,&sentinel)==K_EINVAL);
    assert(watchdog_register("x",10,0,0,&sentinel)==K_EINVAL);
    assert(watchdog_register("x",10,expired,0,0)==K_EINVAL);
    unsigned init_before=atomic_load(&attempts);
    for(unsigned i=0;i<10000;++i)assert(watchdog_manager_init());
    for(unsigned i=0;i<4;++i)assert(!pthread_create(&threads[i],0,registrar,0));
    for(unsigned i=0;i<4;++i)assert(!pthread_join(threads[i],0));
    assert(atomic_load(&attempts)==init_before && !queued && !atomic_load(&g_watchdog.lock.state));
    assert(!preempt_depth);
    for(unsigned nesting=0;nesting<32;++nesting) {
        for(unsigned i=0;i<nesting;++i)sched_preempt_disable();
        assert(watchdog_register("nested",10,expired,0,&sentinel)==K_OK);
        assert(preempt_depth==nesting);
        assert(watchdog_start(sentinel)==K_OK && preempt_depth==nesting);
        assert(watchdog_kick(sentinel)==K_OK && preempt_depth==nesting);
        assert(watchdog_poll(atomic_load(&now))==0 && preempt_depth==nesting);
        watchdog_lock();assert(preempt_depth==nesting+1);
        assert(watchdog_poll(UINT64_MAX)==0 && preempt_depth==nesting+1);
        watchdog_unlock();assert(preempt_depth==nesting);
        assert(watchdog_get_state(sentinel)==WATCHDOG_RUNNING && preempt_depth==nesting);
        assert(watchdog_expiration_count(sentinel)==0 && preempt_depth==nesting);
        assert(watchdog_stop(sentinel)==K_OK && preempt_depth==nesting);
        assert(watchdog_unregister(sentinel)==K_OK && preempt_depth==nesting);
        assert(watchdog_kick(sentinel)==K_ENOENT && preempt_depth==nesting);
        for(unsigned i=0;i<nesting;++i)sched_preempt_enable();
    }
    assert(!preempt_depth);
    puts("Watchdog: eight initializers, all pointer offsets, busy IRQ skip, full-queue retry, stop suppression, 1000 reentrant expiration/kick cycles, 4000 concurrent client lifecycles and 32 nested-preemption depths passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-watchdog-') as directory:
    binary=str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-x','c','-','-o',binary],input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
