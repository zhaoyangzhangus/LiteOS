"""Actual IOVA chooser against an independent page occupancy oracle."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/drivers/core/dma.c').read_text()
source=source[:source.index('static int32_t dma_find_slot(')]
source=source.replace('for (uint32_t i = 0; i < DMA_MAPPING_LIMIT; ++i) {',
    'for (uint32_t i = 0; i < DMA_MAPPING_LIMIT; ++i) { ++checks;')
code=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned checks;
static unsigned preempt_depth;
void sched_preempt_disable(void) {++preempt_depth;}
void sched_preempt_enable(void) {assert(preempt_depth);--preempt_depth;}
''' + source + r'''
static dma_mapping_t maps[DMA_MAPPING_LIMIT];
static dma_segment_t segments[DMA_MAPPING_LIMIT];
static bool occupied[1024];
static void clear(void) {
    memset(g_dma_mappings,0,sizeof(g_dma_mappings));
    memset(maps,0,sizeof(maps)); memset(segments,0,sizeof(segments));
    memset(occupied,0,sizeof(occupied)); g_dma_next_iova=DMA_IOVA_BASE;checks=0;
}
static void interval(unsigned slot,unsigned start,unsigned length) {
    assert(start+length<=1024);
    segments[slot]=(dma_segment_t){.addr=iova_make(DMA_IOVA_BASE+(uint64_t)start*PAGE_SIZE),.length=(uint64_t)length*PAGE_SIZE};
    maps[slot]=(dma_mapping_t){.segments=&segments[slot],.segment_count=1,.mapped_length=segments[slot].length};
    g_dma_mappings[slot]=&maps[slot];
    for(unsigned i=start;i<start+length;++i)occupied[i]=true;
}
static unsigned oracle(unsigned start,unsigned length) {
    for(unsigned p=start;p+length<=1024;++p) {
        bool free=true;
        for(unsigned i=p;i<p+length;++i)if(occupied[i]) { free=false;break; }
        if(free)return p;
    }
    assert(0);return 0;
}
static unsigned rng(unsigned *seed) { *seed=*seed*1664525U+1013904223U;return *seed; }
int main(void) {
    dma_initialize();dma_lock();
    for(unsigned reverse=0;reverse<2;++reverse) {
        clear();
        for(unsigned i=0;i<DMA_MAPPING_LIMIT;++i)interval(i,reverse?DMA_MAPPING_LIMIT-1-i:i,1);
        uint64_t out=UINT64_MAX;
        assert(dma_choose_iova(PAGE_SIZE,&out) && out==DMA_IOVA_BASE+DMA_MAPPING_LIMIT*PAGE_SIZE);
        if(!reverse)assert(checks==DMA_MAPPING_LIMIT);
        else assert(checks<=33152); /* Do not regress reverse-ordered chains. */
        printf("DMA IOVA %s chain: %u slot checks\n",reverse?"reverse":"ascending",checks);
    }
    unsigned seed=31,cases=20000;
    for(unsigned c=0;c<cases;++c) {
        clear();unsigned count=rng(&seed)%257;
        for(unsigned i=0;i<count;++i) {
            unsigned start=(rng(&seed)>>8)%512,length=1+(rng(&seed)>>8)%8;
            interval((i*73)%DMA_MAPPING_LIMIT,start,length); /* Deliberately unsorted and overlapping. */
        }
        unsigned hint=(rng(&seed)>>8)%520,length=1+(rng(&seed)>>8)%9;
        bool unaligned=(rng(&seed)&1)!=0;
        g_dma_next_iova=DMA_IOVA_BASE+(uint64_t)hint*PAGE_SIZE+(unaligned?123:0);
        unsigned expected=oracle(hint+(unaligned?1:0),length);
        uint64_t out=UINT64_MAX;
        assert(dma_choose_iova((uint64_t)length*PAGE_SIZE,&out));
        assert(out==DMA_IOVA_BASE+(uint64_t)expected*PAGE_SIZE);
        assert(g_dma_next_iova==out+(uint64_t)length*PAGE_SIZE);
    }
    clear();uint64_t out=UINT64_MAX;
    g_dma_next_iova=DMA_IOVA_LIMIT-PAGE_SIZE;
    assert(dma_choose_iova(2*PAGE_SIZE,&out) && out==DMA_IOVA_BASE);
    g_dma_next_iova=DMA_IOVA_LIMIT-PAGE_SIZE;
    assert(dma_choose_iova(PAGE_SIZE,&out) && out==DMA_IOVA_LIMIT-PAGE_SIZE && g_dma_next_iova==DMA_IOVA_BASE);
    clear();segments[0]=(dma_segment_t){.addr=iova_make(DMA_IOVA_LIMIT-PAGE_SIZE),.length=PAGE_SIZE};
    maps[0]=(dma_mapping_t){.segments=&segments[0],.segment_count=1,.mapped_length=PAGE_SIZE};g_dma_mappings[0]=&maps[0];
    g_dma_next_iova=DMA_IOVA_LIMIT-PAGE_SIZE;
    assert(dma_choose_iova(PAGE_SIZE,&out) && out==DMA_IOVA_BASE);
    clear();out=UINT64_MAX;
    assert(!dma_choose_iova(0,&out) && out==UINT64_MAX);
    assert(!dma_choose_iova(UINT64_MAX,&out) && out==UINT64_MAX);
    assert(!dma_choose_iova(1,0));
    segments[0]=(dma_segment_t){.addr=iova_make(DMA_IOVA_BASE),.length=DMA_IOVA_LIMIT-DMA_IOVA_BASE};
    maps[0]=(dma_mapping_t){.segments=&segments[0],.segment_count=1,.mapped_length=segments[0].length};
    g_dma_mappings[0]=&maps[0];
    assert(!dma_choose_iova(PAGE_SIZE,&out) && out==UINT64_MAX);
    clear();
    interval(0,0,1);maps[0].mapped_length=0;
    assert(!dma_choose_iova(PAGE_SIZE,&out) && out==UINT64_MAX);
    maps[0].mapped_length=PAGE_SIZE;segments[0].addr=iova_make(UINT64_MAX-1);
    assert(!dma_choose_iova(PAGE_SIZE,&out) && out==UINT64_MAX);
    dma_unlock();
    printf("DMA IOVA: %u occupancy-oracle cases, alignment, exact limit/wrap, invalid lengths and malformed ranges passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-dma-iova-') as directory:
    binary=str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-x','c','-','-o',binary],input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
