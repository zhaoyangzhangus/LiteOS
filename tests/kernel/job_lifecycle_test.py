"""Production Job lifetime: concurrent duplicate detach and exact references."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/resource.c').read_text()
object_source = (root/'kernel/object/object.c').read_text()
object_init = object_source[object_source.index('void object_header_init('):
                            object_source.index('void object_get(')]
wait_source = (root/'kernel/sync/wait.c').read_text()
start = wait_source.index('void wait_queue_init(')
queue_init = wait_source[start:wait_source.index('\n}',start)+2]
code = r'''
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel/spinlock.h>
static void before_resource_lock(spinlock_t *lock);
static void after_resource_unlock(spinlock_t *lock);
''' + source.replace('static void resource_lock(spinlock_t *lock) {',
                     'static void resource_lock(spinlock_t *lock) { before_resource_lock(lock);').replace(
                     'spinlock_unlock(lock);','spinlock_unlock(lock); after_resource_unlock(lock);') + r'''
static atomic_uint lock_entrants,allocations,frees;
static bool parallel_detach,allow_final_destroy=true;
static spinlock_t *detach_lock;
static bool reattach_on_unlock;
static job_t *reattach_target;
static process_t *reattach_process;
static void after_resource_unlock(spinlock_t *lock) {
 if(reattach_on_unlock && lock==detach_lock) {
  reattach_on_unlock=false;
  assert(job_attach_process(reattach_target,reattach_process)==K_OK);
 }
}
static void before_resource_lock(spinlock_t *lock) {
 if(parallel_detach && lock==detach_lock) {
  atomic_fetch_add(&lock_entrants,1);
  while(atomic_load(&lock_entrants)<2)sched_yield();
 }
}
void *kzalloc(size_t size,uint32_t flags) {(void)flags;void *p=calloc(1,size);if(p)atomic_fetch_add(&allocations,1);return p;}
void kfree(void *p) {if(p)atomic_fetch_add(&frees,1);free(p);}
void object_get(void *p) {object_header_t *h=p;assert(atomic_fetch_add(&h->refs.value,1));}
void object_put(void *p) {
 object_header_t *h=p;unsigned count=atomic_fetch_sub(&h->refs.value,1);assert(count);
 if(count==1){assert(allow_final_destroy);h->ops->destroy(p);}
}
''' + 'static void wait_global_init(void) {}\n' + queue_init + object_init + r'''
typedef struct {process_t *process;kstatus_t status;} detach_call_t;
static void *detach_worker(void *opaque) {
 detach_call_t *call=opaque;call->status=job_detach_process(call->process);return 0;
}
int main(void) {
 assert(resource_core_self_test());
 job_limits_t limits={.memory_bytes=123,.process_count=7};job_t *root,*child;
 assert(job_create(0,&limits,&root)==K_OK && !memcmp(&root->limits,&limits,sizeof(limits)));
 assert(list_empty(&root->object.signal_waiters.waiters));
 assert(root->object.signal_waiters.waiters.next==&root->object.signal_waiters.waiters);
 assert(!atomic_load(&root->object.signal_waiters.lock.state) && !atomic_load(&root->object.signal_waiters.sequence));
 assert(job_create(root,0,&child)==K_OK && atomic_load(&root->object.refs.value)==2);
 process_t process;memset(&process,0,sizeof(process));list_init(&process.job_node);
 assert(job_attach_process(child,&process)==K_OK && atomic_load(&child->object.refs.value)==2);
 assert(job_attach_process(child,&process)==K_EBUSY);
 detach_call_t calls[2]={{&process,0},{&process,0}};
 pthread_t threads[2];parallel_detach=true;allow_final_destroy=false;detach_lock=&child->lock;
 for(unsigned i=0;i<2;++i)assert(!pthread_create(&threads[i],0,detach_worker,&calls[i]));
 for(unsigned i=0;i<2;++i)assert(!pthread_join(threads[i],0));
 parallel_detach=false;allow_final_destroy=true;
 assert((calls[0].status==K_OK && calls[1].status==K_EBUSY) || (calls[1].status==K_OK && calls[0].status==K_EBUSY));
 assert(!process.job && list_empty(&child->processes) && process.job_node.next==&process.job_node);
 assert(atomic_load(&child->object.refs.value)==1 && atomic_load(&root->object.refs.value)==2);
 assert(job_detach_process(&process)==K_ENOENT);
 assert(job_attach_process(child,&process)==K_OK);
 reattach_target=root;reattach_process=&process;reattach_on_unlock=true;
 assert(job_detach_process(&process)==K_OK);
 assert(process.job==root && atomic_load(&child->object.refs.value)==1);
 assert(atomic_load(&root->object.refs.value)==3);
 assert(job_detach_process(&process)==K_OK);
 object_put(child);assert(atomic_load(&root->object.refs.value)==1);object_put(root);
 assert(job_detach_process(0)==K_EINVAL && job_attach_process(0,&process)==K_EINVAL);
 assert(atomic_load(&allocations)==atomic_load(&frees));
 puts("Jobs: production self-test, parent/limits/attachment lifetime, duplicate concurrent detach releases one reference, reattachment and balanced allocation passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-job-') as directory:
    binary = str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-x','c','-','-o',binary],
                   input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
