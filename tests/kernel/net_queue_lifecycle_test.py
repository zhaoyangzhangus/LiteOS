"""Production software RX queue: FIFO, wrap, failure balance and concurrent producers."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/drivers/net/queue.c').read_text()
code = r'''
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
static _Thread_local unsigned depth;
void sched_preempt_disable(void) {++depth;}
void sched_preempt_enable(void) {assert(depth);--depth;}
''' + source + r'''
static e1000_software_queue_t queue;
static e1000_rx_packet_t packet(unsigned value) {
    e1000_rx_packet_t p;memset(&p,(unsigned char)value,sizeof(p));
    p.sequence=value;p.payload_length=SOCKET_MAX_PAYLOAD;return p;
}
static void check_packet(const e1000_rx_packet_t *p,unsigned value) {
    e1000_rx_packet_t expected=packet(value);
    assert(!memcmp(p,&expected,sizeof(expected)));
}
static void *producer(void *arg) {
    unsigned id=(unsigned)(uintptr_t)arg;
    for(unsigned i=0;i<3000;++i) {
        e1000_rx_packet_t p=packet(id*10000+i);
        while(!e1000_queue_push(&queue,&p)) {assert(!depth);sched_yield();}
        assert(!depth);
    }
    return 0;
}
int main(void) {
    assert(e1000_packet_queue_self_test());
    for(unsigned nesting=0;nesting<32;++nesting) {
        for(unsigned i=0;i<nesting;++i)sched_preempt_disable();
        e1000_queue_init(&queue);
        e1000_rx_packet_t p=packet(1),out=packet(99),before=out;
        assert(!e1000_queue_push(0,&p) && !e1000_queue_push(&queue,0));
        assert(!e1000_queue_pop(0,&out) && !e1000_queue_pop(&queue,0));
        assert(!e1000_queue_pop(&queue,&out) && !memcmp(&out,&before,sizeof(out)));
        p.payload_length=SOCKET_MAX_PAYLOAD+1;
        assert(!e1000_queue_push(&queue,&p));
        for(unsigned cycle=0;cycle<100;++cycle) {
            for(unsigned i=0;i<E1000_SOFTWARE_QUEUE_DEPTH;++i) {
                p=packet(cycle*E1000_SOFTWARE_QUEUE_DEPTH+i);
                assert(e1000_queue_push(&queue,&p) && depth==nesting);
            }
            assert(!e1000_queue_push(&queue,&p) && depth==nesting);
            assert(queue.count==E1000_SOFTWARE_QUEUE_DEPTH && queue.dropped==cycle+1);
            for(unsigned i=0;i<E1000_SOFTWARE_QUEUE_DEPTH;++i) {
                assert(e1000_queue_pop(&queue,&out) && depth==nesting);
                check_packet(&out,cycle*E1000_SOFTWARE_QUEUE_DEPTH+i);
            }
            assert(!e1000_queue_pop(&queue,&out) && depth==nesting);
        }
        for(unsigned i=0;i<nesting;++i)sched_preempt_enable();
        assert(!depth);
    }
    e1000_queue_init(&queue);
    pthread_t threads[4];unsigned received[4]={0};
    for(unsigned i=0;i<4;++i)assert(!pthread_create(&threads[i],0,producer,(void *)(uintptr_t)i));
    for(unsigned i=0;i<12000;++i) {
        e1000_rx_packet_t out;
        while(!e1000_queue_pop(&queue,&out)) {assert(!depth);sched_yield();}
        unsigned id=out.sequence/10000;
        assert(id<4 && out.sequence%10000==received[id]++ && !depth);
        check_packet(&out,out.sequence);
    }
    for(unsigned i=0;i<4;++i) {
        assert(!pthread_join(threads[i],0));assert(received[i]==3000);
    }
    assert(!queue.count && !atomic_load(&queue.lock.state) && !depth);
    puts("RX queue: 32 nested depths, invalid/full/empty exits, 102400 complete-packet FIFO round trips and 12000 packets from four concurrent producers passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-net-queue-') as directory:
    binary = str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
                    '-I'+str(root/'include'),'-I'+str(root/'kernel/drivers/net'),
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x','c','-','-o',binary],input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
