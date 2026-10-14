"""Actual IPv6 comparisons: byte oracle, unaligned/overlap and page guards."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/net/socket.c').read_text()
helpers = source[source.index('bool socket_address6_equal('):
                 source.index('void socket_lock(')]
native = (root/'kernel/net/socket_test.c').read_text()
assert 'if (!socket_address6_self_test()) return false;' in native
native = native[native.index('static bool socket_address6_self_test('):
                native.index('bool socket_self_test(')]
code = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
''' + helpers + native + r'''
static unsigned cases;
static void check(const uint8_t *a, const uint8_t *b) {
    bool zero=true;
    for (unsigned i=0;i<16;++i) if (a[i]) zero=false;
    assert(socket_address6_zero(a)==zero);
    assert(socket_address6_equal(a,b)==(memcmp(a,b,16)==0));
    ++cases;
}
int main(void) {
    assert(socket_address6_self_test());
    uint8_t a[64], b[64]; uint32_t seed=3;
    for (unsigned offset=0;offset<16;++offset)
    for (unsigned other=0;other<16;++other) {
        memset(a,0,sizeof(a));memset(b,0,sizeof(b));check(a+offset,b+other);
        for (unsigned byte=0;byte<16;++byte)
        for (unsigned bit=0;bit<8;++bit) {
            a[offset+byte]=(uint8_t)(1U<<bit);
            check(a+offset,b+other);
            b[other+byte]=a[offset+byte];check(a+offset,b+other);
            a[offset+byte]=b[other+byte]=0;
        }
    }
    for (unsigned n=0;n<100000;++n) {
        for (unsigned i=0;i<sizeof(a);++i) {
            seed=seed*1664525U+1013904223U;a[i]=(uint8_t)(seed>>24);
            seed=seed*1664525U+1013904223U;b[i]=(uint8_t)(seed>>24);
        }
        unsigned x=n%16,y=(n/16)%16;
        check(a+x,b+y);memcpy(b+y,a+x,16);check(a+x,b+y);
        check(a+x,a+y); /* Read-only overlapping addresses. */
    }
    size_t page=(size_t)sysconf(_SC_PAGESIZE);
    uint8_t *guard=mmap(0,page*3,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(guard!=MAP_FAILED && !mprotect(guard+page,page,PROT_READ|PROT_WRITE));
    uint8_t zero[16]={0};
    for (unsigned i=0;i<32;++i) {
        uint8_t *start=guard+page+i,*end=guard+2*page-16-i;
        memset(start,0,16);memset(end,0,16);check(start,zero);check(end,zero);
        start[15]=128;end[0]=1;check(start,end);
    }
    assert(!munmap(guard,page*3));
    printf("IPv6 addresses: %u byte-oracle, every bit/alignment, overlap and guarded-page cases passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-socket-address-') as directory:
    binary = str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fno-builtin','-mno-sse','-mno-mmx',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x','c','-','-o',binary],input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=60)
