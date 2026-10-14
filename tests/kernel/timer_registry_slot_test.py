"""Actual timer destruction: direct slot, stale metadata recovery and bounds."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'kernel/time/hrtimer.c').read_text()
destroy=source[source.index('static void timer_destroy('):source.index('static void timer_handle_close(')]
previous=destroy.replace('timer_destroy(', 'previous_destroy(')
begin=previous.index('    uint32_t slot = timer->registry_slot;')
end=previous.index('    timer_global_unlock();',begin)
previous=previous[:begin]+'''    for (uint32_t i = 0; i < g_timer_scan_limit; ++i) {
        if (g_timers[i] == timer) g_timers[i] = 0;
    }
    while (g_timer_scan_limit != 0U && g_timers[g_timer_scan_limit - 1U] == 0)
        --g_timer_scan_limit;
'''+previous[end:]
helpers=source[source.index('static uint32_t timer_find_free_slot_locked('):source.index('static void timer_global_lock(')]
def instrument(code):
    return code.replace('g_timers[i] == timer','check_slot(i, timer)').replace(
        'g_timers[slot] == timer','check_slot(slot, timer)')
program='#include "'+str(root/'include/kernel/timer.h')+'"\n#include "'+str(root/'include/kernel/bitmap.h')+'"\n'+r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct {
    object_header_t object;atomic_bool canceled,fired;
    uint64_t deadline_tsc,period_tsc;
} previous_layout_t;
_Static_assert(sizeof(timer_object_t)==sizeof(previous_layout_t),"slot occupies existing padding");
static timer_object_t target,others[TIMER_OBJECT_LIMIT];
static timer_object_t *g_timers[TIMER_OBJECT_LIMIT];
static uint32_t g_timer_scan_limit;
#define TIMER_SLOT_WORDS ((TIMER_OBJECT_LIMIT+63U)/64U)
static uint64_t g_timer_occupied[TIMER_SLOT_WORDS];
''' + helpers + r'''
static unsigned depth,checks,cancels,frees,locks,unlocks;
static bool legacy;
static bool check_slot(unsigned slot,timer_object_t *timer) {
    assert(depth==1&&slot<g_timer_scan_limit);++checks;return g_timers[slot]==timer;
}
kstatus_t timer_cancel(timer_object_t *timer) {
    assert(!depth&&timer==&target);atomic_store(&timer->canceled,true);++cancels;return K_OK;
}
static void timer_global_initialize(void) {assert(!depth);}
static void timer_global_lock(void) {assert(!depth);++depth;++locks;}
static void timer_global_unlock(void) {assert(depth==1);--depth;++unlocks;}
void kfree(void *pointer) {
    assert(!depth&&pointer==&target&&atomic_load(&target.canceled));
    if(!legacy)assert(target.registry_slot==TIMER_OBJECT_LIMIT);
    for(unsigned i=0;i<TIMER_OBJECT_LIMIT;++i) {
        assert(g_timers[i]!=&target);
        if(!legacy)assert(bitmap_test_bit(g_timer_occupied,i)==(g_timers[i]!=0));
    }
    if(!legacy)(void)timer_find_free_slot_locked();
    ++frees;
}
''' + instrument(destroy)+instrument(previous)+r'''
static unsigned bound,position,reported,pattern;
static bool present;
static void reset(bool old) {
    legacy=old;depth=checks=cancels=frees=locks=unlocks=0;
    memset(&target,0,sizeof(target));atomic_init(&target.canceled,false);
    target.registry_slot=reported;g_timer_scan_limit=bound;
    for(unsigned i=0;i<TIMER_OBJECT_LIMIT;++i)
        g_timers[i]=i<bound&&(pattern==0||(pattern==1&&i%2)||i+1==bound)?others+i:0;
    if(present)g_timers[position]=&target;
    bitmap_zero(g_timer_occupied,TIMER_OBJECT_LIMIT);
    for(unsigned i=0;i<TIMER_OBJECT_LIMIT;++i)if(g_timers[i])bitmap_set_bit(g_timer_occupied,i);
}
typedef struct {timer_object_t *slots[TIMER_OBJECT_LIMIT];unsigned limit,cancels,frees,locks,unlocks;} snapshot_t;
static void snapshot(snapshot_t *out) {
    memset(out,0,sizeof(*out));memcpy(out->slots,g_timers,sizeof(g_timers));
    out->limit=g_timer_scan_limit;out->cancels=cancels;out->frees=frees;
    out->locks=locks;out->unlocks=unlocks;
}
int main(void) {
    unsigned cases=0,fast_cases=0;
    uint64_t old_checks=0,new_checks=0;
    for(position=0;position<TIMER_OBJECT_LIMIT;++position)
    for(bound=position+1;bound<=TIMER_OBJECT_LIMIT;++bound)
    for(pattern=0;pattern<3;++pattern)
    for(unsigned metadata=0;metadata<5;++metadata) {
        reported=metadata==0?position:metadata==1?(position+1)%TIMER_OBJECT_LIMIT:
                 metadata==2?TIMER_OBJECT_LIMIT:metadata==3?UINT32_MAX:0;
        present=true;reset(true);previous_destroy(&target);snapshot_t old,actual;snapshot(&old);
        assert(checks==bound);old_checks+=checks;
        reset(false);timer_destroy(&target);snapshot(&actual);
        assert(!memcmp(&old,&actual,sizeof(old))&&!depth&&cancels==1&&frees==1&&locks==1&&unlocks==1);
        if(reported==position) {assert(checks==1);++fast_cases;}
        else assert(checks==bound+(reported<bound));
        new_checks+=checks;++cases;
    }
    present=false;
    for(bound=0;bound<=TIMER_OBJECT_LIMIT;++bound)
    for(pattern=0;pattern<3;++pattern)
    for(unsigned metadata=0;metadata<3;++metadata) {
        reported=metadata==0?0:metadata==1?TIMER_OBJECT_LIMIT:UINT32_MAX;
        reset(true);previous_destroy(&target);snapshot_t old,actual;snapshot(&old);
        reset(false);timer_destroy(&target);snapshot(&actual);
        assert(!memcmp(&old,&actual,sizeof(old))&&!depth&&cancels==1&&frees==1);
        ++cases;
    }
    printf("Timer registry: %u exact destruction/table/bound/cancel/free/lock cases; %u valid direct-slot cases use one lookup; stale/out-of-range/unregistered metadata recovery matches old scan; lookup totals %llu -> %llu; object size unchanged\n",
           cases,fast_cases,(unsigned long long)old_checks,(unsigned long long)new_checks);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-timer-slot-') as directory:
    binary=str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x','c','-','-o',binary],input=program,text=True,check=True)
    subprocess.run([binary],check=True,timeout=60)
