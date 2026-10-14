"""Check actual RTL8126 start register writes, without touching a host NIC."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/drivers/net/rtl8126.c').read_text()
start = source.index('static void rtl8126_write_dma_base(')
end = source.index('static void rtl8126_irq_handler(', start)
body = source[start:source.index('/* Caller owns the controller lifecycle.', start)]
body += source[source.index('static bool rtl8126_start_controller(', start):end]
defines = '\n'.join(re.findall(r'^#define RTL8126_\w+[^\n\\]*$', source, re.M))
defines += '\n' + re.search(r'^#define RTL8126_RX_BUFFER_SIZE.*$',
    (root / 'kernel/drivers/net/core_internal.h').read_text(), re.M).group()
code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stddef.h>
typedef struct { uint64_t rtl_tx_ring_dma,rtl_rx_ring_dma; uint8_t rtl_cmd_after_start; } e1000_state_t;
typedef struct { unsigned width,offset;uint32_t value; } write_t;
static write_t writes[128];
static unsigned count;
static bool start_ok=true;
static void record(unsigned width,unsigned offset,uint32_t value) {
    assert(count<128);writes[count++]=(write_t){width,offset,value};
}
static void rtl8126_write8(const e1000_state_t *s,unsigned r,uint8_t v) {(void)s;record(8,r,v);}
static void rtl8126_write16(const e1000_state_t *s,unsigned r,uint16_t v) {(void)s;record(16,r,v);}
static void rtl8126_write32(const e1000_state_t *s,unsigned r,uint32_t v) {(void)s;record(32,r,v);}
static uint8_t rtl8126_read8(const e1000_state_t *s,unsigned r) {(void)s;(void)r;return 0;}
static uint64_t rtl8126_dma_address(const uint64_t *p) {return *p;}
static bool rtl8126_hardware_start(e1000_state_t *s) {(void)s;return start_ok;}
static void rtl8126_program_mac(e1000_state_t *s) {(void)s;}
static void rtl8126_clear_interrupts(e1000_state_t *s) {(void)s;}
''' + defines + '\n' + body + r'''
int main(void) {
    e1000_state_t s={.rtl_tx_ring_dma=0x123456789000ULL,.rtl_rx_ring_dma=0x234567891000ULL};
    assert(!rtl8126_start_controller(0) && !count);
    e1000_state_t empty={0};assert(!rtl8126_start_controller(&empty) && !count);
    start_ok=false;assert(!rtl8126_start_controller(&s));
    assert(writes[count-1].offset==RTL8126_REG_CFG9346 && !writes[count-1].value);
    count=0;start_ok=true;assert(rtl8126_start_controller(&s));
    unsigned tx_hi=0,tx_lo=0,rx_hi=0,rx_lo=0,lock=0,enable=0,rx=0,tx=0;
    for(unsigned i=0;i<count;++i) {
        write_t w=writes[i];
        if(w.offset==RTL8126_REG_TX_DESC_LO+4) {tx_hi=i;assert(w.value==0x1234);}
        if(w.offset==RTL8126_REG_TX_DESC_LO) {tx_lo=i;assert(w.value==0x56789000);}
        if(w.offset==RTL8126_REG_RX_DESC_LO+4) {rx_hi=i;assert(w.value==0x2345);}
        if(w.offset==RTL8126_REG_RX_DESC_LO) {rx_lo=i;assert(w.value==0x67891000);}
        if(w.offset==RTL8126_REG_CFG9346 && !w.value) lock=i;
        if(w.offset==RTL8126_REG_CHIP_CMD) {enable=i;assert(w.value==12 && w.width==8);}
        if(w.offset==RTL8126_REG_RX_CONFIG) {rx=i;assert(w.value==0x40000f0e);}
        if(w.offset==RTL8126_REG_TX_CONFIG) {tx=i;assert(w.value==0x03000780);}
    }
    assert(tx_hi<tx_lo && rx_hi<rx_lo && tx_lo<lock && rx_lo<lock);
    assert(lock<enable && enable<rx && rx<tx);
}
'''
with tempfile.TemporaryDirectory(prefix='rtl-dma-start-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-x', 'c', '-', '-o', binary], input=code, text=True, check=True)
    subprocess.run([binary], check=True)
print('RTL8126 DMA start: dynamic FIFO, config/enable order, 64-bit ring bases and failure paths PASS')
