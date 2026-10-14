"""Execute the port queue's actual code with deterministic OS primitives."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
header=(root/'kernel/drivers/gpu/nvidia/port/include/nv_port_events.h').read_text()
source=(root/'kernel/drivers/gpu/nvidia/port/nv_port_events.c').read_text()
source=source[:source.index('struct nv_port_event_source {')]
strip=lambda s:'\n'.join(line for line in s.splitlines() if not line.startswith(('#include','#pragma')))
code=r'''
#include <assert.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <sched.h>
typedef int kstatus_t;
enum {K_OK=0,K_EINVAL=-22,K_ENOMEM=-12,K_EBADF=-9,K_ENOENT=-2};
typedef pthread_mutex_t nv_port_spinlock_t;
static atomic_uint allocations,frees;
static atomic_uint irq_allocations;
static atomic_bool fail_allocation;
static _Thread_local unsigned held;
static _Thread_local bool stall_allocation;
static atomic_bool allocation_arrived,allocation_release;
static void nv_port_spin_init(nv_port_spinlock_t *p){assert(!pthread_mutex_init(p,NULL));}
static uint64_t nv_port_spin_lock_irqsave(nv_port_spinlock_t *p){assert(!pthread_mutex_lock(p));assert(!held++);return 5070;}
static void nv_port_spin_unlock_irqrestore(nv_port_spinlock_t *p,uint64_t f){assert(f==5070&&held--==1);assert(!pthread_mutex_unlock(p));}
static void *kmalloc(size_t n,unsigned flags){assert(!held&&!flags);if(atomic_load(&fail_allocation))return NULL;void *p=malloc(n);assert(p);atomic_fetch_add(&allocations,1);if(stall_allocation){atomic_store(&allocation_arrived,true);while(!atomic_load(&allocation_release))sched_yield();}return p;}
static void *kmalloc_try_cached(size_t n,unsigned flags){assert(held==1&&!flags);atomic_fetch_add(&irq_allocations,1);if(atomic_load(&fail_allocation))return NULL;void *p=malloc(n);assert(p);atomic_fetch_add(&allocations,1);return p;}
static void kfree(void *p){assert(p&&!held);atomic_fetch_add(&frees,1);free(p);}
'''+strip(header)+'\n'+strip(source)+r'''
static nv_port_event_queue_t concurrent;
static bool irq_producers;
static void *allocate_during_close(void *arg) {
    stall_allocation=true;
    nv_port_event_record_t record={0};
    assert(nv_port_event_post(arg,&record)==K_EBADF);
    return NULL;
}
static void *produce(void *arg) {
    uint32_t id=(uint32_t)(uintptr_t)arg;
    for(unsigned i=0;i<5000;++i) {
        nv_port_event_record_t r={.parent=id,.index=i};
        int status=irq_producers?nv_port_event_post_irq(&concurrent,&r):nv_port_event_post(&concurrent,&r);
        assert(status==K_OK||status==K_EBADF);
    }
    return NULL;
}
int main(void) {
    nv_port_event_queue_t q;nv_port_event_queue_init(&q);
    nv_port_event_record_t out={0},r={.parent=12,.object=34,.index=56,.info32=78,.info16=90};bool more=true;
    assert(!nv_port_event_poll(&q,false));
    for(unsigned i=0;i<100000;++i)assert(!nv_port_event_post(&q,NULL));
    assert(!atomic_load(&allocations));
    assert(nv_port_event_poll(&q,false)==NV_PORT_EVENT_READ);
    assert(nv_port_event_take(&q,&out,&more)==K_ENOENT&&!more);
    assert(nv_port_event_poll(&q,true)==NV_PORT_EVENT_READ&&!nv_port_event_poll(&q,false));
    atomic_store(&fail_allocation,true);
    assert(nv_port_event_post(&q,&r)==K_ENOMEM&&!nv_port_event_poll(&q,false));
    atomic_store(&fail_allocation,false);
    unsigned before=atomic_load(&irq_allocations);
    assert(!nv_port_event_post_irq(&q,NULL)&&atomic_load(&irq_allocations)==before);
    assert(nv_port_event_poll(&q,true)==NV_PORT_EVENT_READ);
    atomic_store(&fail_allocation,true);
    assert(nv_port_event_post_irq(&q,&r)==K_ENOMEM&&!nv_port_event_poll(&q,false));
    atomic_store(&fail_allocation,false);
    assert(!nv_port_event_post_irq(&q,&r));
    assert(!nv_port_event_take(&q,&out,&more)&&out.object==34&&!more);
    for(unsigned i=0;i<1000;++i){r.index=i;assert(!nv_port_event_post(&q,&r));}
    for(unsigned i=0;i<1000;++i) {
        assert(!nv_port_event_take(&q,&out,&more));
        assert(out.parent==12&&out.object==34&&out.index==i&&out.info32==78&&out.info16==90);
        assert(more==(i!=999));
    }
    assert(!nv_port_event_poll(&q,false));
    nv_port_event_queue_init(&concurrent);
    pthread_t p[4];
    for(unsigned i=0;i<4;++i)assert(!pthread_create(p+i,NULL,produce,(void*)(uintptr_t)i));
    for(unsigned i=0;i<4;++i)assert(!pthread_join(p[i],NULL));
    unsigned next[4]={0};
    for(unsigned i=0;i<20000;++i){assert(!nv_port_event_take(&concurrent,&out,&more));assert(out.parent<4&&out.index==next[out.parent]++);}
    for(unsigned i=0;i<4;++i)assert(next[i]==5000);
    irq_producers=true;
    for(unsigned i=0;i<4;++i)next[i]=0;
    for(unsigned i=0;i<4;++i)assert(!pthread_create(p+i,NULL,produce,(void*)(uintptr_t)i));
    for(unsigned received=0;received<20000;) {
        int status=nv_port_event_take(&concurrent,&out,&more);
        if(status==K_ENOENT){sched_yield();continue;}
        assert(status==K_OK&&out.parent<4&&out.index==next[out.parent]++);
        ++received;
    }
    for(unsigned i=0;i<4;++i){assert(!pthread_join(p[i],NULL));assert(next[i]==5000);}
    for(unsigned i=0;i<4;++i)assert(!pthread_create(p+i,NULL,produce,(void*)(uintptr_t)i));
    nv_port_event_queue_close(&concurrent);
    for(unsigned i=0;i<4;++i)assert(!pthread_join(p[i],NULL));
    assert(nv_port_event_poll(&concurrent,true)==NV_PORT_EVENT_CLOSED);
    assert(nv_port_event_post(&concurrent,NULL)==K_EBADF);
    before=atomic_load(&irq_allocations);
    assert(nv_port_event_post_irq(&concurrent,&r)==K_EBADF&&atomic_load(&irq_allocations)==before);
    assert(nv_port_event_take(&concurrent,&out,&more)==K_EBADF);
    nv_port_event_queue_close(&concurrent);
    for(unsigned i=0;i<3;++i)assert(!nv_port_event_post(&q,&r));
    assert(!pthread_create(p,NULL,allocate_during_close,&q));
    while(!atomic_load(&allocation_arrived))sched_yield();
    nv_port_event_queue_close(&q);
    atomic_store(&allocation_release,true);
    assert(!pthread_join(p[0],NULL));
    assert(atomic_load(&allocations)==atomic_load(&frees));
    assert(nv_port_event_post(NULL,NULL)==K_EINVAL);
    assert(nv_port_event_take(&q,NULL,&more)==K_EINVAL);
    nv_port_event_queue_close(&q);
    assert(!pthread_mutex_destroy(&q.lock)&&!pthread_mutex_destroy(&concurrent.lock));
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='nvidia-event-queue-') as directory:
    base=Path(directory);(base/'test.c').write_text(code)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',str(base/'test.c'),'-o',str(base/'test')],check=True)
    subprocess.run([str(base/'test')],check=True,timeout=15)
print('NVIDIA OS event queue: dataless coalescing without allocation, FIFO/payload/pending, allocation failure, concurrent producers, close race and cleanup passed')
