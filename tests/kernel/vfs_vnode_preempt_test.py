"""Actual vnode lock: preemptible contention, guarded ownership/nesting."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/fs/vfs/internal.h').read_text()
start=source.index('static inline void vfs_state_lock(')
end=source.index('\nsize_t vfs_string_length',start)
core=(root/'kernel/fs/vfs/core.c').read_text()
namespace=core[core.index('static void vfs_lock('):core.index('\nsize_t vfs_string_length')]
program=r'''
#include <assert.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
typedef struct { struct { atomic_uint state; } lock; } vnode_t;
typedef struct {struct {atomic_uint state;} io_lock;} vfs_backend_node_t;
static struct {atomic_uint state;} g_vfs_lock;
static _Thread_local unsigned depth;
static _Thread_local bool deterministic;
static unsigned expected_wait_depth, waited;
static _Thread_local unsigned unlocked_depth;
static atomic_uint unsafe, active;
static unsigned visits;
static void sched_preempt_disable(void) { ++depth; }
static void sched_preempt_enable(void) { assert(depth); --depth; }
static unsigned exchange_probe(atomic_uint *lock,unsigned value,memory_order order) {
 unsigned previous=atomic_exchange_explicit(lock,value,order);
 if (!previous && !depth) ++unsafe;
 return previous;
}
static unsigned load_probe(atomic_uint *lock,memory_order order) {
 if (deterministic) {
  assert(depth==expected_wait_depth); ++waited;
  // Model the remote owner completing while this contender is preemptible.
  atomic_store_explicit(lock,0,memory_order_release);
 }
 return atomic_load_explicit(lock,order);
}
static void store_probe(atomic_uint *lock,unsigned value,memory_order order) {
 assert(depth); unlocked_depth=depth;
 atomic_store_explicit(lock,value,order);
}
#undef atomic_exchange_explicit
#define atomic_exchange_explicit exchange_probe
#undef atomic_load_explicit
#define atomic_load_explicit load_probe
#undef atomic_store_explicit
#define atomic_store_explicit store_probe
''' + source[start:end] + namespace + r'''
#undef atomic_exchange_explicit
#undef atomic_load_explicit
#define atomic_load_explicit(P,O) __atomic_load_n((P),(O))
#undef atomic_store_explicit
#define atomic_store_explicit(P,V,O) __atomic_store_n((P),(V),(O))
static vnode_t vnode;
static vfs_backend_node_t backend;
static void acquire(unsigned kind){
 if(kind==0)vfs_vnode_lock(&vnode);
 else if(kind==1)vfs_backend_lock(&backend);
 else vfs_lock();
}
static void release(unsigned kind){
 if(kind==0)vfs_vnode_unlock(&vnode);
 else if(kind==1)vfs_backend_unlock(&backend);
 else vfs_unlock();
}
static void *contender(void *arg) {
 unsigned kind=(unsigned)(uintptr_t)arg;
 for(unsigned i=0;i<10000;++i) {
  acquire(kind);assert(depth==1);
  assert(atomic_fetch_add(&active,1)==0); ++visits;
  assert(atomic_fetch_sub(&active,1)==1);
  release(kind);assert(!depth);
 }
 return 0;
}
int main(void) {
 atomic_init(&vnode.lock.state,0);
 vfs_vnode_lock(&vnode);assert(depth==1);
 vfs_vnode_unlock(&vnode);assert(!depth && unlocked_depth==1);
 // A failed acquisition drops only its own guard before read-only waiting.
 atomic_store(&vnode.lock.state,1);deterministic=true;expected_wait_depth=0;
 vfs_vnode_lock(&vnode);assert(waited==1 && depth==1);
 vfs_vnode_unlock(&vnode);assert(!depth);
 // Existing outer preemption disable must not be accidentally removed.
 atomic_store(&vnode.lock.state,1);sched_preempt_disable();expected_wait_depth=1;
 vfs_vnode_lock(&vnode);assert(waited==2 && depth==2);
 vfs_vnode_unlock(&vnode);assert(depth==1 && unlocked_depth==2);
 sched_preempt_enable();deterministic=false;
 for(unsigned kind=1;kind<3;++kind){
  atomic_uint *state=kind==1?&backend.io_lock.state:&g_vfs_lock.state;
  atomic_init(state,1);deterministic=true;expected_wait_depth=0;
  acquire(kind);assert(depth==1);release(kind);assert(!depth);
  atomic_store(state,1);sched_preempt_disable();expected_wait_depth=1;
  acquire(kind);assert(depth==2);release(kind);assert(depth==1);
  sched_preempt_enable();deterministic=false;
 }
 pthread_t workers[4];
 for(unsigned kind=0;kind<3;++kind){
  for(unsigned i=0;i<4;++i) assert(!pthread_create(&workers[i],0,contender,(void*)(uintptr_t)kind));
  for(unsigned i=0;i<4;++i) assert(!pthread_join(workers[i],0));
 }
 assert(visits==120000 && waited==6 && !atomic_load(&unsafe) && !atomic_load(&active));
 assert(!depth && !atomic_load(&vnode.lock.state));
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-vnode-preempt-') as directory:
    path=Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-pthread',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('VFS namespace/backend/vnode locks: preemptible remote-owner wait, nested guards, protected acquisition/release and 120000 concurrent visits passed')
