"""Production audio lifecycle with controlled allocation, DMA and backend hooks."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/drivers/audio/core.c').read_text()
source = source.replace('atomic_compare_exchange_strong_explicit(&g_audio_init_state',
                        'counted_cas(&g_audio_init_state')
code = r'''
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdatomic.h>
static _Thread_local unsigned depth,caller_depth;
static _Thread_local int heap_budget=-1,page_budget=-1,map_budget=-1;
static _Thread_local _Bool fail_unmap;
static atomic_uint attempts,heaps,page_count,mappings,callbacks;
static _Bool parallel;
void sched_preempt_disable(void) {++depth;}
void sched_preempt_enable(void) {assert(depth);--depth;}
static _Bool counted_cas(atomic_uint *p,unsigned *expected,unsigned desired,
                         memory_order success,memory_order failure) {
    atomic_fetch_add(&attempts,1);
    if(parallel)while(atomic_load(&attempts)<8)sched_yield();
    return atomic_compare_exchange_strong_explicit(p,expected,desired,success,failure);
}
''' + source + r'''
static device_t device;
static bool allowed(int *budget) {
    if(*budget==0)return false;
    if(*budget>0)--*budget;
    return true;
}
void *kzalloc(size_t n,uint32_t flags) {
    assert(!flags && depth==caller_depth+(n!=sizeof(audio_stream_t)));
    if(!allowed(&heap_budget))return 0;
    void *p=calloc(1,n);assert(p);atomic_fetch_add(&heaps,1);return p;
}
void kfree(void *p) {assert(p && depth<=caller_depth+1);assert(atomic_fetch_sub(&heaps,1));free(p);}
page_t *page_alloc(uint8_t order,page_alloc_flags_t flags) {
    assert(!order && flags==(PAGE_ALLOC_ZERO|PAGE_ALLOC_DMA32) && depth==caller_depth+1);
    if(!allowed(&page_budget))return 0;
    page_t *p=calloc(1,sizeof(*p)+PAGE_SIZE);assert(p);atomic_fetch_add(&page_count,1);return p;
}
void page_free(page_t *p) {assert(p && depth==caller_depth+1);assert(atomic_fetch_sub(&page_count,1));free(p);}
paddr_t page_to_phys(const page_t *p) {assert(p);return paddr_make((uintptr_t)(p+1));}
void *phys_to_direct(paddr_t p) {return (void *)(uintptr_t)p.value;}
void object_get(void *p) {assert(p==&device);assert(atomic_fetch_add(&device.object.refs.value,1));}
void object_put(void *p) {
    assert(p && depth==caller_depth);object_header_t *h=p;
    unsigned before=atomic_fetch_sub(&h->refs.value,1);assert(before);
    if(before==1) {assert(p!=&device && h->ops);h->ops->destroy(p);}
}
kstatus_t dma_map_pages(device_t *d,page_t **p,uint32_t n,enum dma_direction direction,dma_mapping_t *out) {
    assert(d==&device && p && n && depth==caller_depth+1);
    assert(direction==DMA_TO_DEVICE || direction==DMA_FROM_DEVICE);
    if(!allowed(&map_budget))return K_EIO;
    *out=(dma_mapping_t){.device=d,.pages=p,.page_count=n,.direction=direction};
    atomic_fetch_add(&device.object.refs.value,1);atomic_fetch_add(&mappings,1);return K_OK;
}
kstatus_t dma_unmap_checked(dma_mapping_t *m) {
    assert(depth==caller_depth+1 && m && m->device==&device);
    if(fail_unmap)return K_EIO;
    assert(atomic_fetch_sub(&mappings,1));assert(atomic_fetch_sub(&device.object.refs.value,1)>1);
    *m=(dma_mapping_t){0};return K_OK;
}
void dma_sync_for_device(dma_mapping_t *m) {assert(m && depth==caller_depth+1);}
void dma_sync_for_cpu(dma_mapping_t *m) {assert(m && depth==caller_depth+1);}
kstatus_t xhci_audio_stream_disconnect(audio_stream_t *s) {
    assert(s && depth==caller_depth && !atomic_load(&s->lock.state));
    atomic_fetch_add(&callbacks,1);return K_OK;
}
kstatus_t hda_audio_stream_disconnect(audio_stream_t *s) {return xhci_audio_stream_disconnect(s);}
static audio_format_t format={.sample_rate=48000,.channels=2,.sample_format=AUDIO_SAMPLE_S16_LE,
                             .period_frames=128,.period_count=4};
static void clean(void) {
    assert(depth==caller_depth && !atomic_load(&heaps) && !atomic_load(&page_count));
    assert(!atomic_load(&mappings) && atomic_load(&device.object.refs.value)==1);
}
static void *initializer(void *unused) {(void)unused;assert(audio_core_init());return 0;}
static void cycle(void) {
    audio_stream_t *s=0;
    assert(audio_stream_create(&device,AUDIO_PLAYBACK,&s)==K_OK && s);
    assert(audio_stream_configure(s,&format)==K_OK && depth==caller_depth);
    assert(audio_stream_period_bytes(s)==512 && audio_stream_period_page_count(s,0)==1);
    unsigned char input[512],output[512];
    for(unsigned i=0;i<512;++i)input[i]=(unsigned char)(i*17);
    assert(audio_stream_period_write(s,0,input,512)==K_OK);
    assert(audio_stream_period_read(s,0,output,512)==K_OK);
    for(unsigned i=0;i<512;++i)assert(input[i]==output[i]);
    assert(audio_stream_queue(s,0,128)==K_OK && audio_stream_queue(s,1,128)==K_OK);
    assert(audio_stream_queue(s,0,128)==K_EBUSY);
    assert(audio_stream_start(s)==K_OK && audio_stream_configure(s,&format)==K_EBUSY);
    assert(audio_stream_complete(s,0,129)==K_EINVAL && audio_stream_complete(s,2,128)==K_EBUSY);
    assert(audio_stream_complete(s,0,0)==K_OK && audio_stream_complete(s,1,128)==K_OK);
    assert(s->state==AUDIO_STREAM_XRUN && s->underruns==1 && s->completed_frames==256);
    assert(audio_stream_recover(s)==K_EAGAIN);
    assert(audio_stream_queue(s,2,64)==K_OK && audio_stream_recover(s)==K_OK);
    assert(audio_stream_complete(s,2,64)==K_OK && audio_stream_controller_reset(s)==K_OK);
    assert(audio_stream_stop(s)==K_OK && audio_stream_disconnect(s)==K_OK);
    assert(audio_stream_disconnect(s)==K_OK && audio_stream_queue(s,0,1)==K_EDEVREMOVED);
    assert(depth==caller_depth);audio_stream_destroy(s);assert(depth==caller_depth);
}
static void *worker(void *unused) {(void)unused;for(unsigned i=0;i<100;++i)cycle();assert(!depth);return 0;}
int main(void) {
    refcount_init(&device.object.refs,1);pthread_t threads[8];parallel=true;
    for(unsigned i=0;i<8;++i)assert(!pthread_create(&threads[i],0,initializer,0));
    for(unsigned i=0;i<8;++i)assert(!pthread_join(threads[i],0));parallel=false;
    assert(atomic_load(&attempts)==8);
    for(unsigned i=0;i<10000;++i)assert(audio_core_init());
    assert(atomic_load(&attempts)==8);clean();
    for(unsigned nesting=0;nesting<32;++nesting) {
        for(unsigned i=0;i<nesting;++i)sched_preempt_disable();caller_depth=nesting;
        cycle();clean();
        audio_stream_t *unchanged=(void *)(uintptr_t)1;heap_budget=0;
        assert(audio_stream_create(&device,AUDIO_PLAYBACK,&unchanged)==K_ENOMEM);
        assert(unchanged==(void *)(uintptr_t)1);heap_budget=-1;clean();
        for(unsigned layout=0;layout<2;++layout) {
        format.period_frames=layout?4096:128;
        for(unsigned kind=0;kind<3;++kind)
        for(unsigned fail=0;fail<((kind==1 && layout)?16U:4U);++fail) {
            audio_stream_t *s=0;assert(audio_stream_create(&device,AUDIO_CAPTURE,&s)==K_OK);
            if(kind==0)heap_budget=fail;
            if(kind==1)page_budget=fail;
            if(kind==2)map_budget=fail;
            assert(audio_stream_configure(s,&format)==(kind==2?K_EIO:K_ENOMEM));
            heap_budget=page_budget=map_budget=-1;
            assert(s->state==AUDIO_STREAM_NEW && depth==nesting);
            assert(!atomic_load(&page_count) && !atomic_load(&mappings));
            audio_stream_destroy(s);clean();
        }
        }
        format.period_frames=128;
        audio_stream_t *s=0;assert(audio_stream_create(&device,AUDIO_PLAYBACK,&s)==K_OK);
        assert(audio_stream_configure(s,&format)==K_OK);unsigned retained=atomic_load(&heaps);
        fail_unmap=true;
        assert(audio_stream_disconnect(s)==K_EIO && s->state==AUDIO_STREAM_CONFIGURED);
        assert(audio_stream_configure(s,&format)==K_EIO && depth==nesting);
        assert(atomic_load(&heaps)==retained && atomic_load(&page_count)==4 && atomic_load(&mappings)==4);
        fail_unmap=false;assert(audio_stream_disconnect(s)==K_OK);audio_stream_destroy(s);clean();
        for(unsigned i=0;i<nesting;++i)sched_preempt_enable();caller_depth=0;assert(!depth);
    }
    for(unsigned i=0;i<4;++i)assert(!pthread_create(&threads[i],0,worker,0));
    for(unsigned i=0;i<4;++i)assert(!pthread_join(threads[i],0));clean();
    puts("Audio lifecycle: eight initializers, zero repeated CAS, 32 nested depths, every period heap/page/DMA allocation failure, retained failed-unmap ownership, unlocked backend callbacks and 400 concurrent complete lifecycles passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-audio-lifecycle-') as directory:
    binary = str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-ffunction-sections','-fdata-sections',
                    '-Wl,--gc-sections','-x','c','-','-o',binary],input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
