"""Actual root filesystem direct-page predicate must agree with NVMe page rules."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/init/filesystem_root.c').read_text()
start=source.index('            if (page != 0 &&')
end=source.index(' {\n                page_memory = buffer;',start)
condition=source[start:end].strip()[4:-1]
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define PAGE_SIZE 4096U
#define PAGE_FREE 1U
#define PAGE_COMPOUND_HEAD 2U
#define PAGE_COMPOUND_TAIL 4U
typedef struct {uint32_t flags,order;void *memory;} page_t;
static void *page_to_phys(page_t *p) {return p->memory;}
static void *phys_to_direct(void *p) {return p;}
static bool direct(page_t *page,void *buffer,uint32_t sector_count) {
    return '''+condition+r''';
}
int main(void) {
    unsigned char memory[8192];page_t page={0,0,memory};unsigned cases=0;
    assert(!direct(0,memory,8));
    for(unsigned flags=0;flags<8;++flags)for(unsigned order=0;order<8;++order)
    for(unsigned sectors=1;sectors<=17;++sectors)for(unsigned alias=0;alias<2;++alias) {
        page.flags=flags;page.order=order;
        bool expected=!(flags&(PAGE_FREE|PAGE_COMPOUND_TAIL)) && !alias &&
             sectors<=((flags&PAGE_COMPOUND_HEAD)?(8U<<order):8U);
        assert(direct(&page,memory+alias,sectors)==expected);++cases;
    }
    page.flags=PAGE_COMPOUND_TAIL;page.order=0;
    assert(!direct(&page,memory,8));
    puts("NVMe direct pages: 2176 flag/order/size/alias cases; compound tails rejected for safe bounce fallback passed");
    assert(cases==2176);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-nvme-direct-') as directory:
    binary=str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie','-x','c','-',
                    '-o',binary],input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
