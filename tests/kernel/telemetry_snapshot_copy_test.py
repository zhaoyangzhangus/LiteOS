"""Production bulk snapshot versus the prior per-record oracle, with kernel memcpy."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/trace.c').read_text()
code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned depth, copies;
static size_t copied_bytes;
void sched_preempt_disable(void) { ++depth; }
void sched_preempt_enable(void) { assert(depth); --depth; }
void *liteos_memcpy(void *,const void *,size_t);
static void *checked_copy(void *,const void *,size_t);
''' + source.replace('memcpy(', 'checked_copy(') + r'''
x86_cpu_features_t x86_boot_cpu_features;
uint64_t x86_read_tsc(void) { return 123; }
uint64_t x86_tsc_to_ns(uint64_t ticks) { return ticks; }
uint32_t x86_current_cpu_index(void) { return 0; }
static void *checked_copy(void *out,const void *in,size_t bytes) {
    assert(depth>0 && atomic_load(&g_telemetry.lock.state) && bytes);
    ++copies; copied_bytes+=bytes; return liteos_memcpy(out,in,bytes);
}
static size_t oracle(uint64_t after,telemetry_record_t *out,size_t capacity,uint64_t *next) {
    size_t count=0;
    if(!capacity) return 0;
    uint64_t newest=g_telemetry.next_sequence;
    if(after==UINT64_MAX) { *next=newest; return 0; }
    uint64_t first=newest>=TELEMETRY_RING_CAPACITY ? newest-TELEMETRY_RING_CAPACITY+1 : 1;
    if(after+1>first) first=after+1;
    for(uint64_t sequence=first;sequence && sequence<=newest && count<capacity;++sequence) {
        telemetry_record_t record=g_telemetry.records[(sequence-1)%TELEMETRY_RING_CAPACITY];
        if(record.sequence==sequence) out[count++]=record;
    }
    *next=newest; return count;
}
static void seed(uint64_t newest) {
    memset(g_telemetry.records,0,sizeof(g_telemetry.records));
    g_telemetry.next_sequence=newest;
    uint64_t first=newest>=TELEMETRY_RING_CAPACITY ? newest-TELEMETRY_RING_CAPACITY+1 : 1;
    for(uint64_t seq=first;seq && seq<=newest;++seq) {
        telemetry_record_t record={0};
        record.sequence=seq; record.timestamp_tsc=seq*19; record.cpu=(uint32_t)seq;
        record.category=7; record.object_id=seq*23; record.value0=~seq; record.value1=seq*31;
        g_telemetry.records[(seq-1)%TELEMETRY_RING_CAPACITY]=record;
    }
}
int main(void) {
    assert(telemetry_init()); unsigned cases=0;
    telemetry_record_t actual[TELEMETRY_RING_CAPACITY+2], expected[TELEMETRY_RING_CAPACITY+2];
    for(unsigned state=0;state<516;++state) {
        uint64_t newest=state<512 ? state : UINT64_MAX-(state-512)*128ULL;
        seed(newest);
        uint64_t first=newest>=TELEMETRY_RING_CAPACITY ? newest-TELEMETRY_RING_CAPACITY+1 : 1;
        uint64_t afters[]={0,first-1,first,newest/2,newest?newest-1:0,newest,
                          newest==UINT64_MAX?UINT64_MAX:newest+1,UINT64_MAX};
        for(unsigned mode=0;mode<8;++mode) for(size_t cap=0;cap<=258;++cap) {
            size_t capacity=cap==258 ? SIZE_MAX : cap;
            memset(actual,0x5c,sizeof(actual)); memset(expected,0x5c,sizeof(expected));
            uint64_t next=0xdeadbeef, oracle_next=next;
            size_t expected_count=oracle(afters[mode],expected+1,capacity,&oracle_next);
            copies=0; copied_bytes=0;
            depth=state%32;
            size_t count=telemetry_snapshot(afters[mode],actual+1,capacity,&next);
            assert(count==expected_count && next==oracle_next);
            assert(!memcmp(actual,expected,sizeof(actual)));
            assert(depth==state%32 && !atomic_load(&g_telemetry.lock.state));
            assert(copies<=2 && copied_bytes==count*sizeof(*actual));
            if(count) {
                size_t index=(actual[1].sequence-1)%TELEMETRY_RING_CAPACITY;
                assert(copies==(count>TELEMETRY_RING_CAPACITY-index ? 2U : 1U));
            } else assert(!copies);
            ++cases;
        }
    }
    depth=0;
    printf("Telemetry snapshot: %u prior-oracle/canary cases, all ring offsets, empty/partial/full rings, UINT64_MAX, 32 nesting depths and at most two kernel memcpy spans passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-telemetry-copy-') as directory:
    obj = str(Path(directory)/'string.o')
    binary = str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-fno-builtin','-mno-sse','-mno-mmx',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined','-fno-pie',
                    '-Dmemcpy=liteos_memcpy','-Dmemmove=liteos_memmove',
                    '-Dmemset=liteos_memset','-Dmemcmp=liteos_memcmp',
                    '-c',str(root/'kernel/string.c'),'-o',obj],check=True)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x','c','-','-x','none',obj,'-o',binary],
                   input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
