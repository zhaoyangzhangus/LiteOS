"""Actual timeout poll: detach terminal entries without touching subscriptions."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/sync/wait.c').read_text()
def function(name):
    start = source.index(name)
    return source[start:source.index('\n}', start) + 2]

helpers = '\n'.join(function(name) for name in [
    'static void wait_insert_tail(', 'static void wait_remove(',
    'static void waiter_unlink_locked(',
    'static waiter_t *waiter_from_timeout_node(',
    'static bool deadline_reached(', 'static thread_t *waiter_claim_locked(',
    'static thread_t *waiter_claim_pinned_locked(', 'static void wait_wake_pinned('])
helpers = next(line for line in source.splitlines() if line.startswith('enum { WAIT_WAKE_BATCH_COUNT')) + '\n' + helpers
poll = function('void wait_poll_timeouts(').replace(
    'waiter_t *waiter = waiter_from_timeout_node(node);',
    '++visits; waiter_t *waiter = waiter_from_timeout_node(node);')
program = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/wait.h>
typedef struct thread { unsigned index, refs; } thread_t;
static spinlock_t g_timeout_lock;
static list_head_t g_timeout_waiters;
static atomic_uint g_wait_init_state;
static unsigned visits, wakes, lock_depth, wake_counts[2048], acquisitions, pins, peak_pins, last_index;
static uint64_t ticks;
static uint64_t x86_read_tsc(void) { return ++ticks; }
static uint64_t wait_lock(spinlock_t *lock) {
 spinlock_lock(lock); ++lock_depth; ++acquisitions; return 0;
}
static void wait_unlock(spinlock_t *lock, uint64_t flags) {
 assert(!flags && lock_depth); --lock_depth; spinlock_unlock(lock);
}
static void sched_wake(thread_t *thread) {
 assert(!lock_depth && thread->refs==2 && thread->index>=last_index);
 last_index=thread->index; ++wakes; assert(++wake_counts[thread->index]==1);
 // Leave resumed-owner cleanup delayed, as on a busy scheduler.
}
static void object_get(void *p) {
 thread_t *t=p;assert(lock_depth==1 && t->refs==1);++t->refs;
 ++pins;if(pins>peak_pins)peak_pins=pins;
}
static void object_put(void *p) { thread_t *t=p;assert(!lock_depth && t->refs==2 && pins);--t->refs;--pins; }
''' + helpers + poll + r'''
static waiter_t waiters[2048];
static thread_t threads[2048];
static wait_subscription_t subscriptions[2048];
static wait_queue_t queue;
static unsigned list_count(list_head_t *head) {
 unsigned n=0; for(list_head_t *p=head->next;p!=head;p=p->next)++n;return n;
}
int main(void) {
 const unsigned sizes[]={0,1,2,8,15,16,17,31,32,33,256,1024,2048};
 for(unsigned s=0;s<sizeof(sizes)/sizeof(*sizes);++s)
 for(unsigned pattern=0;pattern<5;++pattern) {
  unsigned n=sizes[s], expired=0, future=0;
  spinlock_init(&g_timeout_lock);list_init(&g_timeout_waiters);
  list_init(&queue.waiters);atomic_init(&g_wait_init_state,2);
  memset(waiters,0,sizeof(waiters));memset(wake_counts,0,sizeof(wake_counts));
  for(unsigned i=0;i<n;++i) {
   waiter_t *w=&waiters[i];threads[i].index=i;threads[i].refs=1;w->thread=&threads[i];
   w->subscriptions=&subscriptions[i];w->subscription_count=1;
   unsigned state=pattern==1?WAITER_WOKEN:pattern==2?i%4:WAITER_WAITING;
   atomic_init(&w->state,state);
   w->deadline_tsc=pattern==3?(i%2?101:100):pattern==4?UINT64_MAX:100;
   if(state==WAITER_WAITING) {
    if(deadline_reached(100,w->deadline_tsc))++expired;else ++future;
   }
   list_init(&w->timeout_node);wait_insert_tail(&g_timeout_waiters,&w->timeout_node);
   subscriptions[i].waiter=w;subscriptions[i].queue=&queue;
   list_init(&subscriptions[i].queue_node);
   wait_insert_tail(&queue.waiters,&subscriptions[i].queue_node);
  }
  visits=wakes=acquisitions=pins=peak_pins=last_index=0;wait_poll_timeouts(100);
  assert(wakes==expired && !lock_depth);
  assert(acquisitions==expired/WAIT_WAKE_BATCH_COUNT+1 && !pins && peak_pins<=WAIT_WAKE_BATCH_COUNT);
  assert(list_count(&g_timeout_waiters)==future && list_count(&queue.waiters)==n);
  if(pattern<3)assert(visits==n);
  visits=last_index=0;wait_poll_timeouts(100);
  assert(visits==future && wakes==expired);
  for(unsigned i=0;i<n;++i) {
   assert(threads[i].refs==1);
   waiter_t *w=&waiters[i];
   bool pending=atomic_load(&w->state)==WAITER_WAITING;
   assert((w->timeout_node.next!=&w->timeout_node)==pending);
   assert(subscriptions[i].queue_node.next!=&subscriptions[i].queue_node);
  }
  last_index=0;wait_poll_timeouts(101);
  if(pattern==3)assert(wakes==n && list_empty(&g_timeout_waiters));
  for(unsigned i=0;i<n;++i)waiter_unlink_locked(&waiters[i]);
  assert(list_empty(&g_timeout_waiters) && list_empty(&queue.waiters));
 }
 // Signed deadline arithmetic must retain wraparound behavior.
 assert(!deadline_reached(UINT64_MAX-4,3) && deadline_reached(3,3));
 assert(deadline_reached(4,3) && !deadline_reached(4,UINT64_MAX));
 atomic_store(&g_wait_init_state,0);visits=0;wait_poll_timeouts(100);assert(!visits);
 puts("PASS: timeout pruning, future/infinite deadlines, wrap and 15/16/17 boundaries through 2048; 1024 expired threads: lock acquisitions 1025 -> 65, visits remain 1024");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-timeout-scan-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(root / 'include'), '-fsanitize=address,undefined',
                    '-fno-pie', '-no-pie', str(path / 'test.c'), '-o', str(path / 'test')],
                   check=True)
    subprocess.run([str(path / 'test')], check=True)
    negative = program.replace('wait_wake_pinned(threads[index]);',
                               'wait_wake_pinned(threads[0]);')
    (path / 'test.c').write_text(negative)
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(root / 'include'), '-fsanitize=address,undefined',
                    '-fno-pie', '-no-pie', str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    result = subprocess.run([str(path / 'test')], capture_output=True)
    assert result.returncode != 0, 'Timeout batch duplicated first thread and lost others'
    print('Negative control rejected: duplicate timeout wake and missing batch members')
