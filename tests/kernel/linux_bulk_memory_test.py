"""Actual Linux ABI/socket memory wrappers linked to production kernel strings."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/syscall/linux.c').read_text()
wrappers = source[source.index('static void linux_zero('):
                  source.index('static int64_t linux_copy_status(')]
code = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>
void *liteos_memset(void *,int,size_t);
void *liteos_memmove(void *,const void *,size_t);
''' + wrappers.replace('memset(', 'liteos_memset(').replace('memmove(', 'liteos_memmove(') + r'''
int main(void) {
 unsigned char source[8192],actual[8192],expected[8192];unsigned cases=0;
 for(unsigned i=0;i<sizeof(source);++i)source[i]=(unsigned char)(i*17+3);
 const size_t lengths[]={0,1,7,8,17,63,64,65,255,256,257,511,512,513,4095,4096};
 for(unsigned offset=0;offset<64;++offset)for(unsigned l=0;l<sizeof(lengths)/sizeof(lengths[0]);++l) {
  size_t n=lengths[l];memset(actual,0x75,sizeof(actual));memset(expected,0x75,sizeof(expected));
  linux_zero(actual+offset,n);memset(expected+offset,0,n);assert(!memcmp(actual,expected,sizeof(actual)));
  linux_copy(actual+offset,source+63-offset,n);memmove(expected+offset,source+63-offset,n);
  assert(!memcmp(actual,expected,sizeof(actual)));++cases;
 }
 for(unsigned from=0;from<64;++from)for(unsigned to=0;to<64;++to)for(unsigned l=0;l<sizeof(lengths)/sizeof(lengths[0]);++l) {
  size_t n=lengths[l];memcpy(actual,source,sizeof(actual));memcpy(expected,source,sizeof(expected));
  linux_copy(actual+to,actual+from,n);memmove(expected+to,expected+from,n);
  assert(!memcmp(actual,expected,sizeof(actual)));++cases;
 }
 size_t page=(size_t)sysconf(_SC_PAGESIZE);
 unsigned char *guard=mmap(0,page*3,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 assert(guard!=MAP_FAILED && !mprotect(guard+page,page,PROT_READ|PROT_WRITE));
 for(size_t n=0;n<=page;++n) {
  unsigned char *end=guard+page*2-n;
  linux_zero(end,n);for(size_t i=0;i<n;++i)assert(!end[i]);
  linux_copy(end,source,n);assert(!memcmp(end,source,n));
  linux_copy(actual,end,n);assert(!memcmp(actual,source,n));
 }
 assert(!munmap(guard,page*3));linux_zero(0,0);linux_copy(0,0,0);
 printf("Linux ABI memory: %u aligned/non-overlap/overlap cases, every guard-page length through %zu and zero-length null operations with actual kernel strings passed\n",cases,page);
}
'''
socket_source = (root/'kernel/net/socket.c').read_text()
socket_wrappers = socket_source[socket_source.index('void socket_zero('):
                                socket_source.index('uint32_t socket_stream_write_locked(')]
socket_wrappers = socket_wrappers.replace('socket_zero', 'linux_zero').replace('socket_copy', 'linux_copy')
socket_wrappers = socket_wrappers.replace('memset(', 'liteos_memset(').replace('memmove(', 'liteos_memmove(')
socket_code = code.replace(wrappers.replace('memset(', 'liteos_memset(').replace('memmove(', 'liteos_memmove('),
                           socket_wrappers).replace('Linux ABI memory:', 'Socket memory:')
with tempfile.TemporaryDirectory(prefix='liteos-linux-bulk-') as directory:
    path = Path(directory)
    obj = str(path/'string.o')
    subprocess.run(['cc','-std=c11','-O2','-fno-builtin','-mno-sse','-mno-mmx',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined','-fno-pie',
                    '-Dmemcpy=liteos_memcpy','-Dmemmove=liteos_memmove',
                    '-Dmemset=liteos_memset','-Dmemcmp=liteos_memcmp',
                    '-c',str(root/'kernel/string.c'),'-o',obj],check=True)
    for label, fixture in [('linux',code),('socket',socket_code)]:
        binary = str(path/label)
        subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-pie','-no-pie',
                        '-x','c','-','-x','none',obj,'-o',binary],input=fixture,text=True,check=True)
        subprocess.run([binary],check=True,timeout=30)
