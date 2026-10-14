"""Actual Bluetooth core: nested guards, callback reentry and failure exits."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/drivers/bluetooth/core.c').read_text()
code = r'''
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
static _Thread_local unsigned depth, baseline;
static _Thread_local int fail_alloc;
void sched_preempt_disable(void) { ++depth; }
void sched_preempt_enable(void) { assert(depth); --depth; }
static _Bool checked_acquire(atomic_uint *state, unsigned *expected,
                            unsigned value, memory_order success,
                            memory_order failure) {
    assert(depth==baseline+1);
    return atomic_compare_exchange_strong_explicit(state,expected,value,success,failure);
}
#pragma push_macro("atomic_compare_exchange_strong_explicit")
#undef atomic_compare_exchange_strong_explicit
#define atomic_compare_exchange_strong_explicit checked_acquire
#include <kernel/spinlock.h>
#pragma pop_macro("atomic_compare_exchange_strong_explicit")
''' + source + r'''
static _Thread_local input_event_t events[16];
static _Thread_local unsigned event_count;
static _Thread_local bt_controller_t *observed;
static _Thread_local int fail_input;
static atomic_uint allocations;
void *kzalloc(size_t size, uint32_t flags) {
    (void)flags; assert(depth==baseline);
    if (fail_alloc) return 0;
    void *p=calloc(1,size); assert(p); atomic_fetch_add(&allocations,1); return p;
}
void kfree(void *p) {
    assert(depth==baseline);
    if (p) { atomic_fetch_sub(&allocations,1); free(p); }
}
kstatus_t input_core_push(const input_event_t *event) {
    assert(depth==baseline && (!observed || !atomic_load(&observed->lock.state)));
    if (observed) { bt_controller_stats_t stats; bt_controller_get_stats(observed,&stats); }
    if (fail_input || event_count==16) return K_ENOMEM;
    events[event_count++]=*event; return K_OK;
}
kstatus_t input_core_pop(input_event_t *event) {
    if (!event_count) return K_ENOENT;
    *event=events[0]; --event_count;
    memmove(events,events+1,event_count*sizeof(*events)); return K_OK;
}
struct transport { bt_controller_t *controller; unsigned calls; int fail,complete; };
static kstatus_t send_packet(void *opaque, const uint8_t *packet, size_t length) {
    struct transport *t=opaque;
    assert(depth==baseline && !atomic_load(&t->controller->lock.state));
    bt_controller_stats_t stats; bt_controller_get_stats(t->controller,&stats);
    assert(depth==baseline && length>=4 && (packet[0]==1 || packet[0]==2));
    ++t->calls;
    if (packet[0]==1 && t->complete) {
        uint16_t opcode=(uint16_t)packet[1] | ((uint16_t)packet[2]<<8);
        assert(stats.pending_opcode==opcode);
        assert(bt_hci_complete(t->controller,opcode,0)==K_OK);
    }
    return t->fail ? K_EIO : K_OK;
}
static void cycle(void) {
    bt_controller_t *c=(void *)1; struct transport t={0};
    fail_alloc=1;
    assert(bt_controller_create(send_packet,&t,&c)==K_ENOMEM && !c);
    fail_alloc=0;
    assert(bt_controller_create(0,&t,&c)==K_EINVAL && !c);
    assert(bt_controller_create(send_packet,&t,0)==K_EINVAL);
    assert(bt_controller_create(send_packet,&t,&c)==K_OK); t.controller=c; observed=c;
    assert(bt_controller_start(0)==K_EINVAL);
    assert(bt_hci_submit(c,1,0,0)==K_EBUSY);
    assert(bt_controller_start(c)==K_OK && bt_controller_start(c)==K_OK);
    assert(bt_hci_submit(c,0,0,0)==K_EINVAL);
    assert(bt_hci_submit(c,1,0,1)==K_EINVAL);
    uint8_t parameter=7;
    t.fail=1;
    assert(bt_hci_submit(c,1,&parameter,1)==K_EIO && !c->pending_opcode);
    t.fail=0;
    assert(bt_hci_submit(c,1,0,0)==K_OK);
    assert(bt_hci_submit(c,2,0,0)==K_EBUSY);
    assert(bt_hci_complete(c,2,0)==K_ENOENT);
    assert(bt_hci_complete(c,1,1)==K_EIO && !c->pending_opcode);
    t.complete=1;
    assert(bt_hci_submit(c,3,0,0)==K_OK && !c->pending_opcode);
    uint8_t invalid[]={0x0e,4,1,3,0};
    assert(bt_hci_receive_event(c,invalid,sizeof(invalid))==K_EINVAL);
    uint8_t acl[]={0x40,0x20,4,0,0,0,0x40,0};
    assert(bt_acl_receive(c,acl,sizeof(acl))==K_OK);
    assert(bt_acl_submit(c,acl,sizeof(acl))==K_OK);
    acl[2]=3;
    assert(bt_acl_receive(c,acl,sizeof(acl))==K_EINVAL);
    assert(bt_acl_submit(c,acl,sizeof(acl))==K_EINVAL);
    uint8_t out[BT_GATT_MAX_VALUE]; uint16_t length=0;
    assert(bt_gatt_read_attribute(c,1,out,sizeof(out),&length)==K_ENOENT);
    for (unsigned i=1;i<=BT_GATT_MAX_ATTRIBUTES;++i)
        assert(bt_gatt_add_attribute(c,i,i==1?BT_GATT_PERM_WRITE:BT_GATT_PERM_READ,"ok",2)==K_OK);
    assert(bt_gatt_add_attribute(c,2,BT_GATT_PERM_READ,"ok",2)==K_EBUSY);
    assert(bt_gatt_add_attribute(c,33,BT_GATT_PERM_READ,"ok",2)==K_ENOMEM);
    assert(bt_gatt_read_attribute(c,1,out,sizeof(out),&length)==K_EACCES);
    assert(bt_gatt_read_attribute(c,2,out,1,&length)==K_ENOMEM);
    assert(bt_gatt_read_attribute(c,2,out,sizeof(out),&length)==K_OK && length==2 && !memcmp(out,"ok",2));
    uint8_t report[8]={0,0,4};
    assert(bt_hid_keyboard_report(c,report,sizeof(report),7)==K_OK && event_count==1);
    assert(events[0].code==4 && events[0].value==INPUT_VALUE_PRESS);
    event_count=0; report[2]=0;
    assert(bt_hid_keyboard_report(c,report,sizeof(report),7)==K_OK && event_count==1);
    assert(events[0].code==4 && events[0].value==INPUT_VALUE_RELEASE);
    event_count=0; report[2]=4; report[3]=4;
    assert(bt_hid_keyboard_report(c,report,sizeof(report),7)==K_EINVAL && !event_count);
    report[3]=0; fail_input=1;
    assert(bt_hid_keyboard_report(c,report,sizeof(report),7)==K_ENOMEM); fail_input=0;
    assert(depth==baseline && !atomic_load(&c->lock.state));
    assert(bt_controller_reset(c)==K_OK);
    assert(bt_controller_disconnect(c)==K_OK && bt_controller_disconnect(c)==K_OK);
    assert(bt_controller_start(c)==K_EDEVREMOVED && bt_controller_reset(c)==K_EDEVREMOVED);
    assert(bt_hci_submit(c,1,0,0)==K_EDEVREMOVED && bt_hci_complete(c,1,0)==K_EDEVREMOVED);
    acl[2]=4;
    assert(bt_acl_receive(c,acl,sizeof(acl))==K_EDEVREMOVED && bt_acl_submit(c,acl,sizeof(acl))==K_EDEVREMOVED);
    assert(bt_hid_keyboard_report(c,report,sizeof(report),7)==K_EDEVREMOVED);
    assert(depth==baseline && !atomic_load(&c->lock.state));
    observed=0; bt_controller_destroy(c); bt_controller_destroy(0);
}
static void *worker(void *unused) {
    (void)unused;
    for (unsigned i=0;i<100;++i) { cycle(); assert(!depth); }
    return 0;
}
int main(void) {
    for (baseline=0;baseline<32;++baseline) {
        depth=baseline;
        assert(bluetooth_core_self_test() && depth==baseline && !event_count);
        cycle(); assert(depth==baseline && !event_count);
    }
    baseline=depth=0;
    pthread_t threads[4];
    for(unsigned i=0;i<4;++i) assert(!pthread_create(&threads[i],0,worker,0));
    for(unsigned i=0;i<4;++i) assert(!pthread_join(threads[i],0));
    assert(!atomic_load(&allocations));
    puts("Bluetooth core: 32 nested depths, allocation/packet/GATT/input failure exits, transport/input callback reentry outside lock and 400 concurrent lifecycles passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-bluetooth-') as directory:
    binary = str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-x','c','-','-o',binary],
                   input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
