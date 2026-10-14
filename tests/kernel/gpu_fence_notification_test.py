"""Production fence publication, failure and unlocked/reentrant notification."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/drivers/gpu/core/core.c').read_text()
wrappers = source[source.index('static inline __attribute__((always_inline)) void gpu_fence_lock('):
                  source.index('static bool gpu_fence_is_signaled(')]
apis = source[source.index('kstatus_t gpu_fence_signal('):
              source.index('typedef struct gpu_fence_wait_context')]
code = r'''
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <kernel/gpu.h>
#include <kernel/sched.h>
static _Thread_local unsigned depth,caller_depth;
void sched_preempt_disable(void) {++depth;}
void sched_preempt_enable(void) {assert(depth);--depth;}
static gpu_fence_t fence;
static atomic_uint wakes,notifications;
static bool reenter;
uint32_t wake_all(wait_queue_t *q) {
    assert(q==&fence.waitq && depth==caller_depth);
    if(reenter)assert(!atomic_load(&fence.lock.state));
    assert(atomic_load(&fence.completed_value)!=0 || atomic_load(&fence.status)!=K_OK);
    atomic_fetch_add(&wakes,1);return 0;
}
void object_notify_signaled(void *object) {
    assert(object==&fence && depth==caller_depth);
    atomic_fetch_add(&notifications,1);
    if(reenter) {
        assert(!atomic_load(&fence.lock.state));
        kstatus_t status=atomic_load(&fence.status);
        assert(gpu_fence_signal(&fence,atomic_load(&fence.completed_value))==status);
    }
}
''' + wrappers + apis + r'''
static void reset(void) {
    spinlock_init(&fence.lock);fence.object.type=KOBJECT_TYPE_GPU_FENCE;
    atomic_init(&fence.completed_value,0);atomic_init(&fence.status,K_OK);
    atomic_store(&wakes,0);atomic_store(&notifications,0);
}
static void balanced(kstatus_t actual,kstatus_t expected,unsigned count) {
    assert(actual==expected && depth==caller_depth && !atomic_load(&fence.lock.state));
    assert(atomic_load(&wakes)==count && atomic_load(&notifications)==count);
}
static void *signaler(void *arg) {
    unsigned id=(unsigned)(uintptr_t)arg;
    for(unsigned i=0;i<5000;++i) {
        assert(gpu_fence_signal(&fence,(uint64_t)id*5000+i+1)==K_OK && !depth);
    }
    return 0;
}
int main(void) {
    reenter=true;
    for(unsigned nesting=0;nesting<32;++nesting) {
        for(unsigned i=0;i<nesting;++i)sched_preempt_disable();
        caller_depth=nesting;reset();
        balanced(gpu_fence_signal(0,1),K_EINVAL,0);
        balanced(gpu_fence_fail(0,K_EIO),K_EINVAL,0);
        balanced(gpu_fence_fail(&fence,K_OK),K_EINVAL,0);
        fence.object.type=0;
        balanced(gpu_fence_signal(&fence,1),K_EINVAL,0);
        balanced(gpu_fence_fail(&fence,K_EIO),K_EINVAL,0);
        fence.object.type=KOBJECT_TYPE_GPU_FENCE;
        balanced(gpu_fence_signal(&fence,0),K_OK,0);
        balanced(gpu_fence_signal(&fence,1),K_OK,1);
        balanced(gpu_fence_signal(&fence,1),K_OK,1);
        balanced(gpu_fence_signal(&fence,0),K_OK,1);
        balanced(gpu_fence_signal(&fence,UINT64_MAX),K_OK,2);
        balanced(gpu_fence_signal(&fence,2),K_OK,2);
        assert(atomic_load(&fence.completed_value)==UINT64_MAX);
        balanced(gpu_fence_fail(&fence,K_EIO),K_EIO,3);
        balanced(gpu_fence_fail(&fence,K_EDEVLOST),K_EIO,3);
        balanced(gpu_fence_signal(&fence,UINT64_MAX),K_EIO,3);
        assert(atomic_load(&fence.completed_value)==UINT64_MAX);
        reset();balanced(gpu_fence_fail(&fence,K_EDEVLOST),K_EDEVLOST,1);
        balanced(gpu_fence_signal(&fence,1),K_EDEVLOST,1);
        assert(!atomic_load(&fence.completed_value));
        for(unsigned i=0;i<nesting;++i)sched_preempt_enable();
        assert(!depth);caller_depth=0;
    }
    reset();reenter=false;pthread_t threads[4];
    for(unsigned i=0;i<4;++i)assert(!pthread_create(&threads[i],0,signaler,(void *)(uintptr_t)i));
    for(unsigned i=0;i<4;++i)assert(!pthread_join(threads[i],0));
    assert(atomic_load(&fence.completed_value)==20000 && atomic_load(&fence.status)==K_OK);
    assert(atomic_load(&wakes)>0 && atomic_load(&wakes)<=20000);
    assert(atomic_load(&wakes)==atomic_load(&notifications));
    assert(!depth && !atomic_load(&fence.lock.state));
    puts("GPU fence: 32 nested depths, invalid/unchanged/extreme values, first-failure preservation, unlocked notification reentry and 20000 concurrent signals passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-gpu-fence-') as directory:
    binary = str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-x','c','-','-o',binary],
                   input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
