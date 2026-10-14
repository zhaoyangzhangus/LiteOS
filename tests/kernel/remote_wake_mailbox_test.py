import pathlib
import subprocess
import tempfile

source = pathlib.Path('kernel/sched/core.c').read_text()
methods = source[source.index('static bool scheduler_remote_wake_publish('):source.index('static bool scheduler_command_drain_local_limit(')]
release = source[source.index('static inline void scheduler_thread_mailbox_lock('):source.index('static bool scheduler_command_ring_try_pop(')]
release = release.replace('scheduler_thread_mailbox_lock(thread);',
                         'scheduler_thread_mailbox_lock(thread); assert(!held_locks); ++held_locks; atomic_fetch_add(&lock_acquisitions,1);')
release = release.replace('scheduler_thread_mailbox_unlock(thread);',
                         'assert(held_locks==1); --held_locks; scheduler_thread_mailbox_unlock(thread);')
methods = methods.replace('scheduler_thread_mailbox_lock(thread);',
                         'scheduler_thread_mailbox_lock(thread); assert(!held_locks); ++held_locks; atomic_fetch_add(&lock_acquisitions,1);')
methods = methods.replace('scheduler_thread_mailbox_unlock(thread);',
                         'assert(held_locks==1); --held_locks; scheduler_thread_mailbox_unlock(thread);')
program = r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>
#include <sched.h>
#define THREAD_BLOCKED 2U
#define SCHED_UNLIKELY(x) (x)
typedef struct thread {
 atomic_bool remote_wake_queued;
 atomic_uint remote_wake_lock, block_epoch, state;
 struct thread *remote_wake_next;
 uint32_t remote_wake_epoch, owner_cpu;
 atomic_int refs;
 struct thread *command_release_next;
 uint32_t command_release_count;
} thread_t;
typedef struct {
 thread_t * _Atomic remote_wake_head;
 atomic_bool command_pending;
 thread_t *command_release_head,*command_release_tail;
 atomic_bool command_release_pending;
} scheduler_cpu_t;
static scheduler_cpu_t g_cpus[2];
static uint32_t g_cpu_count=2;
static _Thread_local uint32_t current_cpu;
static _Thread_local unsigned held_locks;
static atomic_uint lock_acquisitions;
static _Thread_local bool irq=true;
static uint64_t scheduler_irq_save(void) { bool before=irq;irq=false;return before?1ULL<<9:0; }
static void scheduler_irq_restore(uint64_t flags) { irq=(flags&(1ULL<<9))!=0; }
static bool scheduler_current_cpu(uint32_t *cpu) { *cpu=current_cpu;return true; }
static bool sched_preempt_disabled(void) { return false; }
static bool x86_tlb_shootdown_active(void) { return false; }
static bool object_try_get(thread_t *t) { atomic_fetch_add(&t->refs,1); return true; }
static void object_put(thread_t *t) { assert(!held_locks);assert(atomic_fetch_sub(&t->refs,1)>1); }
static bool scheduler_complete_wake_local(scheduler_cpu_t *c, thread_t *t,
                                         uint32_t e, bool check) {
 (void)c; assert(check);
 if(atomic_load(&t->block_epoch)!=e || atomic_load(&t->state)!=THREAD_BLOCKED) return false;
 atomic_store(&t->state,3U); return true;
}
''' + release + methods + r'''
#define stress_cpu g_cpus[1]
static thread_t stress_threads[4];
static atomic_uint finished;
static atomic_bool command_done;
static void *produce(void *arg) {
 thread_t *t=arg;
 for(unsigned epoch=1;epoch<=5000;++epoch) {
  atomic_store(&t->block_epoch,epoch);
  atomic_store(&t->state,THREAD_BLOCKED);
  assert(scheduler_remote_wake_publish(&stress_cpu,t,epoch));
  while(atomic_load(&t->state)==THREAD_BLOCKED || atomic_load(&t->remote_wake_queued)) sched_yield();
 }
 atomic_fetch_add(&finished,1); return NULL;
}
static void *consume(void *arg) {
 (void)arg;current_cpu=1;
 while(atomic_load(&finished)!=4 || atomic_load(&stress_cpu.remote_wake_head)) {
  uint64_t flags=scheduler_irq_save();
  scheduler_remote_wake_drain(1,&stress_cpu);
  scheduler_irq_restore(flags);
  scheduler_command_release_drain(1);sched_yield();
 }
 do {scheduler_command_release_drain(1);sched_yield();}
 while(!atomic_load(&command_done)||atomic_load(&stress_cpu.command_release_pending));
 return NULL;
}
static void *stale_commands(void *arg) {
 (void)arg;current_cpu=0;
 for(unsigned n=0;n<100000;++n) {
  thread_t *t=&stress_threads[n%4];
  assert(object_try_get(t));uint64_t flags=scheduler_irq_save();
  scheduler_command_release_enqueue_local(&g_cpus[0],t);
  scheduler_irq_restore(flags);scheduler_command_release_drain(0);
 }
 atomic_store(&command_done,true);
 do {scheduler_command_release_drain(0);sched_yield();}
 while(atomic_load(&finished)!=4||atomic_load(&g_cpus[0].command_release_pending));
 return NULL;
}
int main(void) {
 current_cpu=1;
 scheduler_cpu_t *cpu=&g_cpus[1];
 thread_t *threads=calloc(10000,sizeof(*threads)); assert(threads);
 for(unsigned i=0;i<10000;++i) {
  thread_t *t=&threads[i]; t->refs=1; t->owner_cpu=1;
  atomic_init(&t->block_epoch,7); atomic_init(&t->state,THREAD_BLOCKED);
  assert(scheduler_remote_wake_publish(cpu,t,7));
  assert(scheduler_remote_wake_publish(cpu,t,7)); assert(t->refs==2);
 }
 /* Reblocking while the old node is pending must preserve the new wake. */
 atomic_store(&threads[0].block_epoch,8);
 assert(!scheduler_remote_wake_publish(cpu,&threads[0],7));
 assert(scheduler_remote_wake_publish(cpu,&threads[0],8));
 unsigned locks_before=atomic_load(&lock_acquisitions);
 uint64_t flags=scheduler_irq_save();scheduler_remote_wake_drain(1,cpu);
 assert(atomic_load(&lock_acquisitions)-locks_before==10000);
 for(unsigned i=0;i<10000;++i)
  assert(threads[i].refs==2&&threads[i].command_release_count==1);
 scheduler_irq_restore(flags);scheduler_command_release_drain(1);
 assert(!atomic_load(&cpu->remote_wake_head));
 for(unsigned i=0;i<10000;++i) {
  assert(threads[i].refs==1 && atomic_load(&threads[i].state)==3);
  assert(!atomic_load(&threads[i].remote_wake_queued));
 }
 atomic_store(&threads[0].state,THREAD_BLOCKED);
 atomic_store(&threads[0].block_epoch,9);
 assert(scheduler_remote_wake_publish(cpu,&threads[0],9));
 atomic_store(&threads[0].block_epoch,10);
 flags=scheduler_irq_save();scheduler_remote_wake_drain(1,cpu);
 scheduler_irq_restore(flags);scheduler_command_release_drain(1);
 assert(atomic_load(&threads[0].state)==THREAD_BLOCKED && threads[0].refs==1);
 free(threads);
 pthread_t producers[4], consumer,commands;
 for(unsigned i=0;i<4;++i) { stress_threads[i].refs=1; stress_threads[i].owner_cpu=1; }
 assert(!pthread_create(&consumer,NULL,consume,NULL));
 assert(!pthread_create(&commands,NULL,stale_commands,NULL));
 for(unsigned i=0;i<4;++i) assert(!pthread_create(&producers[i],NULL,produce,&stress_threads[i]));
 for(unsigned i=0;i<4;++i) assert(!pthread_join(producers[i],NULL));
 assert(!pthread_join(consumer,NULL));
 assert(!pthread_join(commands,NULL));
 for(unsigned i=0;i<4;++i) {
  assert(stress_threads[i].refs==1 && !atomic_load(&stress_threads[i].remote_wake_queued));
  assert(!stress_threads[i].command_release_count&&!stress_threads[i].command_release_next);
  assert(!atomic_load(&stress_threads[i].remote_wake_lock));
 }
 for(unsigned i=0;i<2;++i)
  assert(!g_cpus[i].command_release_head&&!g_cpus[i].command_release_tail&&
         !atomic_load(&g_cpus[i].command_release_pending));
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-wake-mailbox-') as directory:
    binary = str(pathlib.Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x', 'c', '-', '-o', binary], input=program, text=True, check=True)
    subprocess.run([binary], check=True, timeout=30)
print('Remote wake: 10000 nodes, coalescing, reblocking, stale epochs and reference balance passed')
print('Mailbox transfer: 10000 nodes require 10000 lock acquisitions instead of 20000; destruction remains deferred')
print('Concurrent wake: four producers, 20000 blocking generations and node reuse passed')
print('Integrated release: 100000 concurrent stale-command refs, actual delayed FIFOs, shared-lock ownership and final reference balance passed')
