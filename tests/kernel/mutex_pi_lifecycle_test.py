"""Whole production mutex implementation with controlled scheduler/wait hooks."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/sync/mutex.c').read_text()
code = r'''
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
static atomic_uint attempts;
static _Bool parallel;
static _Bool counted_cas(atomic_uint *p,unsigned *expected,unsigned value,
                        memory_order success,memory_order failure) {
 atomic_fetch_add(&attempts,1);
 if(parallel)while(atomic_load(&attempts)<8)sched_yield();
 return atomic_compare_exchange_strong_explicit(p,expected,value,success,failure);
}
''' + source.replace('atomic_compare_exchange_strong_explicit(&g_pi_init_state',
                     'counted_cas(&g_pi_init_state') + r'''
static _Thread_local thread_t *current;
static unsigned wakes,waits;
static bool release_owner;
thread_t *sched_current_thread(void) {return current;}
void sched_initialize_test_thread(thread_t *thread,uint64_t tid,uint8_t cls,uint8_t priority) {
 memset(thread,0,sizeof(*thread));thread->tid=tid;
 thread->sched_class=thread->base_sched_class=cls;
 thread->rt_priority=thread->base_rt_priority=priority;
 list_init(&thread->owned_mutexes);
}
void sched_set_effective_priority(thread_t *thread,uint8_t cls,uint8_t priority) {
 assert(atomic_load(&g_pi_lock.state));thread->sched_class=cls;thread->rt_priority=priority;
}
void wait_queue_init(wait_queue_t *queue) {
 spinlock_init(&queue->lock);list_init(&queue->waiters);atomic_init(&queue->sequence,0);
}
uint32_t wake_all(wait_queue_t *queue) {(void)queue;assert(!atomic_load(&g_pi_lock.state));++wakes;return 0;}
kstatus_t wait_on_queue(wait_queue_t *queue,bool (*predicate)(void *),void *context,uint64_t timeout) {
 (void)queue;(void)timeout;++waits;
 kmutex_wait_context_t *wait=context;kmutex_t *mutex=wait->mutex;
 assert(!atomic_load(&g_pi_lock.state) && current->pi_blocked_on==mutex);
 if(mutex->pi_enabled)assert(mutex->owner->sched_class==SCHED_CLASS_RT && mutex->owner->rt_priority==current->rt_priority);
 if(release_owner) {
  thread_t *saved=current;current=mutex->owner;
  assert(kmutex_unlock(mutex)==K_OK);current=saved;
  assert(predicate(context));return K_OK;
 }
 assert(!predicate(context));return K_ETIMEDOUT;
}
static void *initializer(void *unused) {(void)unused;pi_initialize();assert(!atomic_load(&g_pi_lock.state));return 0;}
int main(void) {
 pthread_t threads[8];parallel=true;
 for(unsigned i=0;i<8;++i)assert(!pthread_create(&threads[i],0,initializer,0));
 for(unsigned i=0;i<8;++i)assert(!pthread_join(threads[i],0));
 parallel=false;assert(atomic_load(&attempts)==8 && kmutex_pi_self_test());
 thread_t low,high;
 sched_initialize_test_thread(&low,10,SCHED_CLASS_FAIR,0);
 sched_initialize_test_thread(&high,11,SCHED_CLASS_RT,2);
 kmutex_t mutex;memset(&mutex,0,sizeof(mutex));
 assert(kmutex_try_lock(&mutex)==K_EINVAL && kmutex_unlock(&mutex)==K_EINVAL);
 for(unsigned enabled=0;enabled<2;++enabled) {
  kmutex_init(&mutex,enabled);current=0;
  assert(kmutex_try_lock(&mutex)==K_EPERM && kmutex_lock(&mutex,0)==K_EPERM);
  current=&low;assert(kmutex_try_lock(&mutex)==K_OK && mutex.owner==&low);
  assert(kmutex_try_lock(&mutex)==K_EBUSY && kmutex_lock(&mutex,0)==K_EBUSY);
  current=&high;assert(kmutex_try_lock(&mutex)==K_EAGAIN && kmutex_unlock(&mutex)==K_EPERM);
  release_owner=false;assert(kmutex_lock(&mutex,100)==K_ETIMEDOUT);
  assert(!high.pi_blocked_on && mutex.owner==&low && low.sched_class==SCHED_CLASS_FAIR);
  assert(mutex.pi_waiters.next==&mutex.pi_waiters);
  release_owner=true;assert(kmutex_lock(&mutex,100)==K_OK && mutex.owner==&high);
  assert(!high.pi_blocked_on && low.sched_class==SCHED_CLASS_FAIR);
  assert(kmutex_unlock(&mutex)==K_OK && !mutex.owner);
  assert(low.owned_mutexes.next==&low.owned_mutexes && high.owned_mutexes.next==&high.owned_mutexes);
 }
 /* Real two-mutex owner cycle must fail before invoking the wait hook. */
 kmutex_t first,second;kmutex_init(&first,true);kmutex_init(&second,true);
 current=&low;assert(kmutex_try_lock(&first)==K_OK);
 current=&high;assert(kmutex_try_lock(&second)==K_OK);
 high.pi_blocked_on=&first;unsigned before=waits;current=&low;
 assert(kmutex_lock(&second,0)==K_EBUSY && waits==before);
 high.pi_blocked_on=0;assert(kmutex_unlock(&first)==K_OK);
 current=&high;assert(kmutex_unlock(&second)==K_OK);
 for(unsigned i=0;i<10000;++i)kmutex_init(&mutex,true);
 assert(atomic_load(&attempts)==8 && !atomic_load(&g_pi_lock.state) && waits==4 && wakes==6);
 puts("Mutex PI: eight concurrent initializers, production chained donation/restoration self-test, try-lock/ownership, PI-on/off timeout/handoff, deadlock cycle and 10000 initialized-path calls without CAS passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-mutex-pi-') as directory:
    binary = str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-x','c','-','-o',binary],
                   input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
