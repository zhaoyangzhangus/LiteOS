"""Actual size classifier versus the original linear first-fit oracle."""
from pathlib import Path
import re
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/mm/kmalloc.c').read_text()
defines='\n'.join(re.search(r'^#define '+name+r'\s+[^\n]+',source,re.M).group(0)
                  for name in ['KMEM_PAYLOAD_OFFSET','KMEM_TRAILER_SIZE','KMEM_CLASS_COUNT'])
start=source.index('static const uint16_t g_slot_sizes[')
slots=source[start:source.index('};',start)+2]
start=source.index('static uint16_t class_index_for_size(')
functions=source[start:source.index('static void *header_payload(',start)]
code=r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
''' + defines+'\n'+slots+'\n'+functions + r'''
static void check(size_t size) {
    uint16_t expected=KMEM_CLASS_COUNT;
    for(uint16_t i=0;i<KMEM_CLASS_COUNT;++i)
        if(size<=class_capacity(i)) { expected=i;break; }
    assert(class_index_for_size(size)==expected);
}
int main(void) {
    for(size_t size=0;size<=8192;++size)check(size);
    for(unsigned bit=0;bit<sizeof(size_t)*8;++bit) {
        size_t value=(size_t)1<<bit;
        check(value);check(value-1);check(value+1);check(SIZE_MAX-value);
    }
    uint64_t seed=17;
    for(unsigned i=0;i<1000000;++i) {
        seed=seed*UINT64_C(6364136223846793005)+1;
        check((size_t)seed);check((size_t)(seed&4095));
    }
    check(SIZE_MAX);
    puts("kmalloc class: all sizes 0..8192, word-size/overflow boundaries and 2000000 random first-fit oracle cases PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-kmem-classes-') as directory:
    binary=str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x','c','-','-o',binary],input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
