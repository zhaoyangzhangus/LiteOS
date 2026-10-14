"""MAC enable/readback and real TX submission with a fake MMIO controller."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/drivers/net/rtl8126.c').read_text()
model = (root / 'kernel/drivers/net/core_internal.h').read_text()
defines = '\n'.join(re.findall(r'^#define RTL8126_\w+[^\n\\]*$', source + '\n' + model, re.M))
helper = source[source.index('static bool rtl8126_ensure_mac_enabled('):
                source.index('static bool rtl8126_start_controller(')]
tx = source[source.index('bool rtl8126_transmit_state('):
            source.index('bool rtl8126_link_up_state(')]
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
typedef struct {uint32_t options1,options2;uint64_t address;} rtl8126_descriptor_t;
typedef struct {
    bool initialized;
    rtl8126_descriptor_t *rtl_tx_ring;
    uint8_t *rtl_tx_buffers;
    uint16_t rtl_tx_next;
    uint8_t rtl_cmd_last_before_enable,rtl_cmd_last_after_enable;
    uint32_t rtl_enable_attempts,rtl_enable_failures,rtl_tx_count;
    unsigned rtl_tx_ring_dma,rtl_tx_buffer_dma;
} e1000_state_t;
static uint8_t command;
static unsigned writes, delays, doorbells, syncs, mode;
static uint8_t rtl8126_read8(const e1000_state_t *s,unsigned r) {
    (void)s;assert(r==0x37);return command;
}
static void rtl8126_write8(const e1000_state_t *s,unsigned r,uint8_t value) {
    (void)s;assert(r==0x37);++writes;
    if(mode==0 || (mode==2 && writes==2))command=value;
}
static void rtl8126_write16(const e1000_state_t *s,unsigned r,uint16_t value) {
    (void)s;assert(r==0x90 && value==1);++doorbells;
}
static void rtl8126_delay_ms(uint16_t value) {assert(value==1);++delays;}
static void dma_sync_for_cpu(unsigned *dma) {(void)dma;++syncs;}
static void dma_sync_for_device(unsigned *dma) {(void)dma;++syncs;}
static void dma_wmb(void) {}
static void rtl8126_copy(void *d,const void *s,size_t n) {memcpy(d,s,n);}
''' + defines + '\n' + helper + tx + r'''
int main(void) {
    e1000_state_t s={0};
    assert(!rtl8126_ensure_mac_enabled(0));
    for(unsigned c=0;c<256;++c) {
        command=c;writes=delays=0;mode=0;s=(e1000_state_t){0};
        bool valid=c!=255 && !(c&16);
        assert(rtl8126_ensure_mac_enabled(&s)==valid);
        if(!valid)assert(!writes && !delays);
        else {
            assert((command&0x8c)==12);
            assert((command&~0x8c)==(c&~0x8c));
            assert(writes==(((c&0x8c)==12)?0:1));
        }
    }
    command=0;writes=delays=0;mode=2;s=(e1000_state_t){0};
    assert(rtl8126_ensure_mac_enabled(&s) && writes==2 && delays==1);
    assert(s.rtl_enable_attempts==1 && !s.rtl_enable_failures);
    static rtl8126_descriptor_t ring[64];
    static uint8_t buffers[64*2048];
    const uint8_t packet[60]={0x12,0x34};
    s=(e1000_state_t){.initialized=true,.rtl_tx_ring=ring,.rtl_tx_buffers=buffers};
    command=0;writes=delays=0;mode=1;
    assert(!rtl8126_transmit_state(&s,packet,sizeof(packet)));
    assert(writes==2 && delays==1 && !syncs && !doorbells);
    assert(!s.rtl_tx_next && !s.rtl_tx_count && !ring[0].options1);
    assert(s.rtl_enable_failures==1);
    mode=0;writes=0;
    assert(rtl8126_transmit_state(&s,packet,sizeof(packet)));
    assert(writes==1 && doorbells==1 && s.rtl_tx_next==1 && s.rtl_tx_count==1);
    assert(ring[0].options1==(0xb0000000|sizeof(packet)));
    assert(!memcmp(buffers,packet,sizeof(packet)));
    ring[1].options1=1U<<31;
    assert(!rtl8126_transmit_state(&s,packet,sizeof(packet)) && doorbells==1);
}
'''
with tempfile.TemporaryDirectory(prefix='rtl-mac-enable-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-x', 'c', '-', '-o', binary], input=code, text=True, check=True)
    subprocess.run([binary], check=True)
print('RTL8126 MAC: 256 command states, bounded settling retry, no DMA/false success on failure, real TX enqueue PASS')
