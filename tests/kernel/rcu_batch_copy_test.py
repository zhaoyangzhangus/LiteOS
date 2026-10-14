"""Actual RCU ring detach: every head/count/budget against the old loop."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'kernel/sync/rcu.c').read_text()
helper=source[source.index('static uint32_t rcu_take_callbacks_locked('):source.index('uint32_t rcu_poll(')]
program=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define RCU_CALLBACK_CAPACITY 128U
typedef void (*rcu_callback_fn)(void *);
typedef struct {rcu_callback_fn callback;void *argument;} rcu_callback_t;
typedef struct {
    rcu_callback_t callbacks[128];uint32_t head,tail,count;
} queue_t;
static queue_t g_rcu;
static unsigned copies,bytes;
static rcu_callback_t *output;
static void *checked_copy(void *destination,const void *source,size_t length) {
    uintptr_t begin=(uintptr_t)g_rcu.callbacks,end=begin+sizeof(g_rcu.callbacks);
    uintptr_t address=(uintptr_t)source,target=(uintptr_t)destination;
    assert(length&&length%sizeof(rcu_callback_t)==0);
    assert(address>=begin&&address<=end&&length<=end-address);
    assert(target>=(uintptr_t)output&&target<=(uintptr_t)(output+128));
    assert(length<=(uintptr_t)(output+128)-target);
    ++copies;bytes+=(unsigned)length;return memcpy(destination,source,length);
}
#define memcpy checked_copy
''' + helper + r'''
#undef memcpy
static uint32_t previous_take(rcu_callback_t *out,uint32_t budget) {
    uint32_t count=0;
    while(count<budget&&g_rcu.count!=0) {
        out[count++]=g_rcu.callbacks[g_rcu.head];
        g_rcu.head=(g_rcu.head+1)%RCU_CALLBACK_CAPACITY;
        --g_rcu.count;
    }
    return count;
}
static void first(void *argument) {(void)argument;}
static void second(void *argument) {(void)argument;}
int main(void) {
    for(unsigned n=0;n<128;++n)
        g_rcu.callbacks[n]=(rcu_callback_t){n%3==0?0:n%3==1?first:second,
                                            (void *)(uintptr_t)(n*37+1)};
    uint64_t cases=0,old_updates=0,new_updates=0,span_copies=0;
    for(unsigned head=0;head<128;++head)
    for(unsigned count=0;count<=128;++count)
    for(unsigned b=0;b<=130;++b) {
        unsigned budget=b==130?UINT32_MAX:b;
        rcu_callback_t expected[130],actual[130];
        memset(expected,0x99,sizeof(expected));memset(actual,0x99,sizeof(actual));
        g_rcu.head=head;g_rcu.tail=(head+count)%128;g_rcu.count=count;
        unsigned taken=previous_take(expected+1,budget);queue_t old;
        memcpy(&old,&g_rcu,sizeof(old));
        g_rcu.head=head;g_rcu.tail=(head+count)%128;g_rcu.count=count;
        output=actual+1;copies=bytes=0;
        assert(rcu_take_callbacks_locked(output,budget)==taken);
        assert(!memcmp(expected,actual,sizeof(actual))&&!memcmp(&old,&g_rcu,sizeof(old)));
        assert(taken==(count<budget?count:budget));
        assert(bytes==taken*sizeof(rcu_callback_t));
        assert(copies==(taken?1U+(head+taken>128):0));
        assert(copies<=2&&g_rcu.tail==(head+count)%128);
        old_updates+=taken;new_updates+=taken!=0;span_copies+=copies;++cases;
    }
    assert(old_updates>new_updates);
    printf("RCU batch: %llu exact head/count/budget/wrap/full/empty/null-callback/output-canary cases; logical index updates %llu -> %llu, at most two copy spans; %llu total bounded spans\n",
           (unsigned long long)cases,(unsigned long long)old_updates,
           (unsigned long long)new_updates,(unsigned long long)span_copies);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-rcu-batch-') as directory:
    binary=str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x','c','-','-o',binary],input=program,text=True,check=True)
    subprocess.run([binary],check=True,timeout=60)
