"""Actual bounce copy branches and kernel memcpy, including failed-read isolation."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/init/filesystem_root.c').read_text()
allocation_start=source.index('if (page == 0) {')
allocation=source[allocation_start:source.index('vector.base = page_memory;',allocation_start)]
assert 'PAGE_ALLOC_ZERO' not in allocation
assert 'PAGE_ALLOC_DMA32' in allocation
assert source.index('filesystem_copy(page_memory, buffer,',allocation_start) < \
       source.index('status = io_submit(&request);',allocation_start)
helper=source[source.index('static void filesystem_copy('):
              source.index('static void filesystem_log_source(')]
start=source.index('            if (opcode == IO_WRITE) {',source.index('if (page == 0) {'))
write=source[start:source.index('\n        }\n        vector.base',start)]
start=source.index('    if (status == K_OK && opcode == IO_READ && owns_page) {')
read=source[start:source.index('\ndone:',start)]
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t UINT8;
typedef size_t UINTN;
#define IO_READ 1
#define IO_WRITE 2
#define IO_FLUSH 3
#define K_OK 0
void *liteos_memcpy(void *,const void *,size_t);
static unsigned copies;
static void *counted_copy(void *out,const void *in,size_t n) {
    ++copies;return liteos_memcpy(out,in,n);
}
'''+helper.replace('__builtin_memcpy(', 'counted_copy(')+r'''
static void write_copy(int opcode,void *page_memory,const void *buffer,unsigned sector_count) {
'''+write+r'''
}
static void read_copy(int status,int opcode,bool owns_page,void *buffer,const void *page_memory,unsigned sector_count) {
'''+read+r'''
}
int main(void) {
    unsigned char input[8256],output[8256],expected[8256];unsigned cases=0;
    for(unsigned i=0;i<sizeof(input);++i)input[i]=(unsigned char)(i*17+3);
    for(unsigned sectors=1;sectors<=16;++sectors)
    for(unsigned a=0;a<16;++a)for(unsigned b=0;b<16;++b) {
        unsigned n=sectors*512;
        for(unsigned opcode=IO_READ;opcode<=IO_FLUSH;++opcode) {
            memset(output,0x75,sizeof(output));memset(expected,0x75,sizeof(expected));copies=0;
            if(opcode==IO_WRITE)memcpy(expected+b,input+a,n);
            write_copy(opcode,output+b,input+a,sectors);
            assert(!memcmp(output,expected,sizeof(output)) && copies==(opcode==IO_WRITE));++cases;
            for(unsigned success=0;success<2;++success)for(unsigned owns=0;owns<2;++owns) {
                bool copy=success && owns && opcode==IO_READ;
                memset(output,0x75,sizeof(output));memset(expected,0x75,sizeof(expected));copies=0;
                if(copy)memcpy(expected+b,input+a,n);
                read_copy(success?K_OK:-5,opcode,owns,output+b,input+a,sectors);
                assert(!memcmp(output,expected,sizeof(output)) && copies==copy);++cases;
            }
        }
    }
    printf("NVMe bounce: %u 1..16-sector/alignment/opcode/ownership/failure cases, exact bulk copies and untouched failed-read buffers passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-nvme-bounce-') as directory:
    path=Path(directory);obj=str(path/'string.o');binary=str(path/'test')
    subprocess.run(['cc','-std=c11','-O2','-fno-builtin','-mno-sse','-mno-mmx',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined','-fno-pie',
                    '-Dmemcpy=liteos_memcpy','-Dmemmove=liteos_memmove',
                    '-Dmemset=liteos_memset','-Dmemcmp=liteos_memcmp',
                    '-c',str(root/'kernel/string.c'),'-o',obj],check=True)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x','c','-','-x','none',obj,'-o',binary],input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=60)
