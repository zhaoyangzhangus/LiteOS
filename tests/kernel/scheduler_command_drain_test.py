"""Production command drains: FIFO, references, wrap and captured batches."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/sched/core.c').read_text()
begin = source.index('static inline void scheduler_command_drain_ring_local(')
end = source.index('static bool scheduler_remote_wake_publish(', begin)
publish_begin = source.index('static inline bool scheduler_command_publish_snapshot(')
publish_end = source.index('static bool scheduler_command_ring_try_push(', publish_begin)
program = '#include "' + str(root / 'kernel/sched/internal.h') + '"\n' + r'''
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
static atomic_uint *observed_head,*observed_tail;
static sched_cmd_t *observed_slots;
static uint32_t initial_head,initial_tail,mask,seen,expected_count;
static bool append,stress;
static scheduler_cpu_t cpu;
static sched_cmd_t command(uint32_t n) {
    return (sched_cmd_t){.object=(uintptr_t)n+1U,.op=n%9U+1U,
        .generation=(n%3U==0U?SCHED_CMD_REMOVE_NO_REF:0U)|(n&0x7fffffffU)};
}
static void scheduler_command_consume_local(uint32_t id,scheduler_cpu_t *owner,
                                            sched_cmd_t message,bool reference) {
    assert(id==3&&owner==&cpu);
    sched_cmd_t expected=command(stress?seen:initial_head+seen);
    assert(message.object==expected.object&&message.op==expected.op&&
           message.generation==expected.generation);
    assert(reference==(message.op!=SCHED_CMD_REMOVE||
                       !(message.generation&SCHED_CMD_REMOVE_NO_REF)));
    if(!stress) {
        assert(seen<expected_count);
        assert(atomic_load_explicit(observed_head,memory_order_relaxed)==initial_head);
        if(append&&seen==0) {
            observed_slots[initial_tail&mask]=command(initial_tail);
            atomic_store_explicit(observed_tail,initial_tail+1U,memory_order_release);
        }
    }
    ++seen;
}
''' + source[publish_begin:publish_end] + source[begin:end] + r'''
static sched_cmd_ring_t primary;
static sched_cmd_overflow_ring_t overflow;
static void drain(bool large) {
    if(large)scheduler_command_drain_overflow_local(3,&cpu,&overflow);
    else scheduler_command_drain_primary_local(3,&cpu,&primary);
}
static void select_ring(bool large) {
    observed_head=large?&overflow.head:&primary.head;
    observed_tail=large?&overflow.tail:&primary.tail;
    observed_slots=large?overflow.slots:primary.slots;
    mask=large?SCHED_CMD_OVERFLOW_MASK:SCHED_CMD_MASK;
}
static void fixture(bool large,uint32_t start,uint32_t count,bool add) {
    select_ring(large);
    initial_head=start;initial_tail=start+count;expected_count=count;
    append=add;seen=0;
    atomic_store(observed_head,start);atomic_store(observed_tail,start+count);
    for(uint32_t n=0;n<count;++n)observed_slots[(start+n)&mask]=command(start+n);
    drain(large);
    assert(seen==count&&atomic_load(observed_head)==start+count);
    assert(atomic_load(observed_tail)==start+count+(add&&count!=0));
    if(add&&count!=0) {
        initial_head=start+count;expected_count=1;seen=0;append=false;
        drain(large);
        assert(seen==1&&atomic_load(observed_head)==initial_head+1U);
    }
}
static void *produce(void *unused) {
    (void)unused;
    for(uint32_t n=0;n<100000U;++n) {
        uint32_t tail=atomic_load_explicit(observed_tail,memory_order_relaxed);
        while((uint32_t)(tail-atomic_load_explicit(observed_head,memory_order_acquire))>mask)
            __asm__ volatile("pause");
        sched_cmd_t message=command(n);
        assert(scheduler_command_publish_snapshot(observed_tail,observed_slots,mask,
            atomic_load_explicit(observed_head,memory_order_acquire),tail,
            (thread_t *)message.object,message.op,message.generation));
    }
    return 0;
}
int main(void) {
    unsigned cases=0;
    for(unsigned large=0;large<2;++large) {
        unsigned capacity=large?SCHED_CMD_OVERFLOW_SLOTS:SCHED_CMD_SLOTS;
        for(unsigned offset=0;offset<capacity;++offset)
        for(unsigned count=0;count<=capacity;++count)
        for(unsigned add=0;add<2;++add) {
            if(add&&(count==0||count==capacity))continue;
            fixture(large,offset,count,add);++cases;
            fixture(large,UINT32_MAX-offset,count,add);++cases;
        }
        select_ring(large);stress=true;seen=0;
        atomic_store(observed_head,UINT32_MAX-1024U);
        atomic_store(observed_tail,UINT32_MAX-1024U);
        pthread_t producer;assert(!pthread_create(&producer,0,produce,0));
        while(seen<100000U)drain(large);
        assert(!pthread_join(producer,0));
        assert(atomic_load(observed_head)==atomic_load(observed_tail));
        stress=false;
    }
    printf("Command drains: %u empty/full/offset/wrap/append cases; 200000 concurrent FIFO messages; reference flags and deferred head publication passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-command-drain-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-pthread', '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I' + str(root / 'include'), '-x', 'c', '-', '-o', binary],
                   input=program, text=True, check=True)
    subprocess.run([binary], check=True, timeout=60)
