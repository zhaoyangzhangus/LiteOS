"""Production counted completions: ready fast path and exact token accounting."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/sync/wait.c').read_text()
source = source[source.index('static bool completion_try_consume('):]
program = r'''
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <kernel/wait.h>
static atomic_uint waits,wakes;
static void (*before_wait)(void *);
void wait_queue_init(wait_queue_t *queue) { memset(queue,0,sizeof(*queue)); }
uint32_t wake_one(wait_queue_t *queue) { (void)queue;atomic_fetch_add(&wakes,1);return 1; }
kstatus_t wait_on_queue(wait_queue_t *queue,bool (*ready)(void*),void *context,uint64_t timeout) {
 (void)queue;assert(timeout==123);atomic_fetch_add(&waits,1);
 if(before_wait) { void (*hook)(void*)=before_wait;before_wait=0;hook(context); }
 return ready(context)?K_OK:K_ETIMEDOUT;
}
''' + source + r'''
static completion_t shared;
static atomic_uint consumed;
static void arrive(void *context) { complete(context); }
static void *producer(void *unused) {
 (void)unused;for(unsigned i=0;i<10000;++i)complete(&shared);return 0;
}
static void *consumer(void *unused) {
 (void)unused;
 for(unsigned i=0;i<10000;++i) {
  kstatus_t status;
  while((status=completion_wait(&shared,0))==K_ETIMEDOUT)sched_yield();
  assert(status==K_OK);atomic_fetch_add(&consumed,1);
 }
 return 0;
}
int main(void) {
 assert(completion_wait(0,0)==K_EINVAL);
 completion_t c;completion_init(&c,100000);
 for(unsigned i=0;i<100000;++i)assert(completion_wait(&c,i%2?0:UINT64_MAX)==K_OK);
 assert(!atomic_load(&c.count) && !atomic_load(&waits));
 assert(completion_wait(&c,0)==K_ETIMEDOUT && !atomic_load(&waits));
 before_wait=arrive;
 assert(completion_wait(&c,123)==K_OK && !atomic_load(&c.count));
 assert(atomic_load(&waits)==1 && atomic_load(&wakes)==1 && !before_wait);
 assert(completion_wait(&c,123)==K_ETIMEDOUT && atomic_load(&waits)==2);
 complete(&c);assert(completion_wait(&c,123)==K_OK && atomic_load(&waits)==2);
 completion_init(&shared,0);atomic_store(&waits,0);atomic_store(&wakes,0);
 pthread_t producers[4],consumers[4];
 for(unsigned i=0;i<4;++i) {
  assert(!pthread_create(&consumers[i],0,consumer,0));
  assert(!pthread_create(&producers[i],0,producer,0));
 }
 for(unsigned i=0;i<4;++i) {
  assert(!pthread_join(producers[i],0));assert(!pthread_join(consumers[i],0));
 }
 assert(atomic_load(&consumed)==40000 && !atomic_load(&shared.count));
 assert(atomic_load(&wakes)==40000 && !atomic_load(&waits));
 assert(completion_wait(&shared,0)==K_ETIMEDOUT);
 puts("PASS: 100000 ready token consumes without wait calls, publication race, empty polls and 40000 exact tokens across eight threads");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-completion-token-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-pthread', '-I' + str(root / 'include'),
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
