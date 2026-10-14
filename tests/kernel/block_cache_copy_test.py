"""Actual block-copy wrapper linked with the freestanding kernel strings."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'kernel/fs/pagecache/cache.c').read_text()
start=source.index('static VOID copy_bytes(')
wrapper=source[start:source.index('\n}',start)+2]
program=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef void VOID;typedef uint8_t UINT8;typedef uint32_t UINT32;
void *liteos_memmove(void *,const void *,size_t);
static unsigned calls;
static __attribute__((unused)) void *tracked_move(void *to,const void *from,size_t n) {
    ++calls;return liteos_memmove(to,from,n);
}
''' + wrapper.replace('memmove(', 'tracked_move(') + r'''
int main(void) {
    UINT8 source[4128],dest[4128],expected[4128];
    for(unsigned i=0;i<sizeof(source);++i) source[i]=(UINT8)(i*17);
    for(unsigned n=0;n<=4096;++n) {
        unsigned alignment=n%8;
        memset(dest,0xa5,sizeof(dest));memset(expected,0xa5,sizeof(expected));
        memcpy(expected+alignment+8,source+13,n);
        calls=0;copy_bytes(dest+alignment+8,source+13,n);
        assert(calls==1 && !memcmp(dest,expected,sizeof(dest)));
    }
    for(unsigned offset=0;offset<32;++offset) {
        for(unsigned direction=0;direction<2;++direction) {
            memcpy(dest,source,sizeof(dest));memcpy(expected,source,sizeof(expected));
            unsigned from=direction?offset:0,to=direction?0:offset;
            memmove(expected+to,expected+from,4096);
            calls=0;copy_bytes(dest+to,dest+from,4096);
            assert(calls==1 && !memcmp(dest,expected,sizeof(dest)));
        }
    }
    calls=0;copy_bytes(NULL,NULL,0);assert(calls==1);
    puts("Block copy: production integer-only strings, 0..4096 lengths, misalignment, canaries and both overlaps passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-block-copy-') as directory:
    path=Path(directory);obj=path/'string.o';(path/'test.c').write_text(program)
    subprocess.run(['cc','-O2','-fno-builtin','-mno-sse','-mno-mmx','-fno-tree-vectorize',
                    '-I'+str(root/'include'),'-Dmemcpy=liteos_memcpy',
                    '-Dmemmove=liteos_memmove','-Dmemset=liteos_memset',
                    '-Dmemcmp=liteos_memcmp','-c',str(root/'kernel/string.c'),'-o',str(obj)],check=True)
    subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    '-fno-pie','-no-pie',str(path/'test.c'),str(obj),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True,timeout=20)
