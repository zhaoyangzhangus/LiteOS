"""Actual child VM creation retains the parent snapshot against exit teardown."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/process/process.c').read_text()
clone = source[source.index('static kstatus_t process_create_vm('):
               source.index('kstatus_t process_create_named(')]
release = source[source.index('void process_release_runtime_resources('):
                 source.index('static bool process_destruction_self_test_round(')]
fixture = r'''
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#define K_EINVAL -22
#define PROCESS_FLAG_RESOURCES_RELEASED 2U
typedef int kstatus_t;
typedef struct { atomic_uint refs; unsigned value; } vm_space_t;
typedef struct { uint32_t flags; uint64_t pid; pthread_mutex_t lock; vm_space_t *vm; unsigned handles,linux_fds; } process_t;
static _Thread_local unsigned held;
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition=PTHREAD_COND_INITIALIZER;
static bool entered,allow,pause_clone;
static int failure;
static atomic_uint destroyed,gets,vm_puts,clones,creates,handles,fds;
static void process_thread_lock(process_t *p) { assert(!held);pthread_mutex_lock(&p->lock);++held; }
static void process_thread_unlock(process_t *p) { assert(held==1);--held;pthread_mutex_unlock(&p->lock); }
static void vm_space_get(vm_space_t *v) { assert(held && atomic_load(&v->refs));++gets;atomic_fetch_add(&v->refs,1); }
static void vm_space_put(vm_space_t *v) {
 assert(!held && atomic_load(&v->refs));++vm_puts;
 if(atomic_fetch_sub(&v->refs,1)==1) { ++destroyed;free(v); }
}
static int vm_space_create(vm_space_t **out) {
 assert(!held);++creates;if(!out)return K_EINVAL;if(failure)return failure;
 *out=malloc(sizeof(**out));assert(*out);atomic_init(&(*out)->refs,1);(*out)->value=0;return 0;
}
static int vm_space_clone_cow(vm_space_t *v,vm_space_t **out) {
 assert(!held && v);++clones;if(!out)return K_EINVAL;
 if(pause_clone) {
  pthread_mutex_lock(&gate);entered=true;pthread_cond_broadcast(&condition);
  while(!allow)pthread_cond_wait(&condition,&gate);
  pthread_mutex_unlock(&gate);
 }
 assert(atomic_load(&v->refs) && v->value==1234);
 if(failure)return failure;
 *out=malloc(sizeof(**out));assert(*out);atomic_init(&(*out)->refs,1);(*out)->value=v->value;return 0;
}
static void handle_table_destroy(unsigned *table) { assert(!held && table);++handles; }
static void linux_fd_table_destroy(unsigned *table) { assert(!held && table);++fds; }
static void file_lock_release_owner(uint64_t owner) { (void)owner; assert(!held); }
'''
checks = r'''
typedef struct { process_t *parent; vm_space_t *child; int status; } job_t;
static void *clone_job(void *opaque) {
 job_t *job=opaque;job->status=process_create_vm(job->parent,&job->child);return NULL;
}
int main(void) {
 for(unsigned round=0;round<100;++round)for(unsigned mode=0;mode<3;++mode) {
  process_t parent={0};pthread_mutex_init(&parent.lock,NULL);
  parent.vm=malloc(sizeof(*parent.vm));assert(parent.vm);atomic_init(&parent.vm->refs,1);parent.vm->value=1234;
  destroyed=gets=vm_puts=clones=creates=handles=fds=0;entered=allow=false;pause_clone=true;
  failure=mode==0?0:mode==1?-12:-5;job_t job={.parent=&parent};
  pthread_t worker;assert(!pthread_create(&worker,NULL,clone_job,&job));
  pthread_mutex_lock(&gate);while(!entered)pthread_cond_wait(&condition,&gate);pthread_mutex_unlock(&gate);
  process_release_runtime_resources(&parent);
  assert(!parent.vm && !atomic_load(&destroyed) && atomic_load(&gets)==1 && atomic_load(&vm_puts)==1);
  pthread_mutex_lock(&gate);allow=true;pthread_cond_broadcast(&condition);pthread_mutex_unlock(&gate);
  assert(!pthread_join(worker,NULL));
  assert(job.status==failure && atomic_load(&destroyed)==1 && atomic_load(&vm_puts)==2 && atomic_load(&clones)==1);
  if(!failure) { assert(job.child && job.child->value==1234);vm_space_put(job.child); }
  else assert(!job.child);
  assert(atomic_load(&handles)==1 && atomic_load(&fds)==1);
  process_release_runtime_resources(&parent);assert(atomic_load(&handles)==1);
  pthread_mutex_destroy(&parent.lock);
 }
 pause_clone=false;failure=0;
 process_t empty={0};pthread_mutex_init(&empty.lock,NULL);
 vm_space_t *child=(void*)1;unsigned before=atomic_load(&clones);
 assert(process_create_vm(&empty,&child)==K_EINVAL && child==(void*)1 && atomic_load(&clones)==before);
 assert(process_create_vm(&empty,NULL)==K_EINVAL);
 pthread_mutex_destroy(&empty.lock);
 child=NULL;assert(process_create_vm(NULL,&child)==0 && child);vm_space_put(child);
 assert(process_create_vm(NULL,NULL)==K_EINVAL);
 process_t live={0};pthread_mutex_init(&live.lock,NULL);
 live.vm=malloc(sizeof(*live.vm));assert(live.vm);atomic_init(&live.vm->refs,1);live.vm->value=1234;
 assert(process_create_vm(&live,NULL)==K_EINVAL && atomic_load(&live.vm->refs)==1);
 process_release_runtime_resources(&live);pthread_mutex_destroy(&live.lock);
 puts("300 clone/exit interleavings: success/ENOMEM/EIO, null parent/VM/output and balanced references passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-clone-vm-pin-') as directory:
    path = Path(directory)
    def run(actual):
        (path / 'test.c').write_text(fixture + actual + release + checks)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        str(path / 'test.c'), '-o', str(path / 'test')], check=True)
        return subprocess.run([str(path / 'test')], capture_output=True, text=True)
    result = run(clone)
    assert result.returncode == 0, result.stdout + result.stderr
    print(result.stdout.strip())
    negative = clone.replace('if (source != 0) vm_space_get(source);', 'if (false) vm_space_get(source);')
    negative = negative.replace('vm_space_put(source);', '(void)source;')
    result = run(negative)
    assert result.returncode != 0 and 'Assertion' in result.stderr
    print('Negative control rejected: parent teardown destroys the borrowed clone source')
assert 'kstatus_t status = process_create_vm(parent, &process->vm);' in source
