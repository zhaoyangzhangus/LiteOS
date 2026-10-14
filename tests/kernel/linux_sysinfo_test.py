"""Execute physical-memory sysinfo translation and exact x86-64 layout."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root/'kernel/syscall/linux.c').read_text()
begin = source.index('static int64_t linux_sysinfo(')
function = source[begin:source.index('static int64_t linux_getrandom(', begin)]
begin = source.index('static int64_t linux_copy_status(')
copy_status = source[begin:source.index('static bool linux_fd_valid(', begin)]
program = r'''
#include <assert.h>
#include <string.h>
#include <kernel/base.h>
#include <uapi/linux_abi.h>
#include <uapi/time.h>
#define LINUX_EFAULT K_EFAULT
#define __user
static uint64_t total=1048576, available=524288;
static int memory_error,clock_error,copies;
static kstatus_t page_memory_get_stats(uint64_t *pages,uint64_t *free_pages) {
 *pages=total;*free_pages=available;return memory_error;
}
static kstatus_t kernel_clock_read(uint32_t id,os_timespec_t *time) {
 assert(id==OS_CLOCK_MONOTONIC);time->seconds=123;time->nanoseconds=456;return clock_error;
}
static void linux_zero(void *out,size_t size){memset(out,0,size);}
static kstatus_t copy_to_user(void *out,const void *in,size_t size) {
 assert(size==112);if(!out)return K_EACCES;++copies;memcpy(out,in,size);return K_OK;
}
''' + copy_status + function + r'''
int main(void) {
 struct {linux_sysinfo_t info;unsigned char guard[16];} output;
 memset(&output,0xa5,sizeof(output));
 assert(sizeof(output.info)==112 && __builtin_offsetof(linux_sysinfo_t,totalram)==32);
 assert(__builtin_offsetof(linux_sysinfo_t,procs)==80);
 assert(linux_sysinfo((uintptr_t)&output.info)==K_OK);
 assert(output.info.totalram==4294967296ULL && output.info.freeram==2147483648ULL);
 assert(output.info.mem_unit==1 && output.info.uptime==123 && copies==1);
 const unsigned char *bytes=(const void*)&output.info;
 for(unsigned i=80;i<88;++i)assert(bytes[i]==0);
 for(unsigned i=108;i<112;++i)assert(bytes[i]==0);
 for(unsigned i=0;i<16;++i)assert(output.guard[i]==0xa5);
 assert(linux_sysinfo(0)==K_EFAULT);
 memory_error=K_EIO;assert(linux_sysinfo((uintptr_t)&output.info)==K_EIO);memory_error=0;
 clock_error=K_EIO;assert(linux_sysinfo((uintptr_t)&output.info)==K_EIO);clock_error=0;
 total=UINT64_MAX;assert(linux_sysinfo((uintptr_t)&output.info)==K_EOVERFLOW);
 total=10;available=11;assert(linux_sysinfo((uintptr_t)&output.info)==K_EOVERFLOW);
 assert(copies==1);
}
'''
with tempfile.TemporaryDirectory(prefix='linux-sysinfo-') as directory:
    base = Path(directory)
    (base/'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I'+str(root/'include'), str(base/'test.c'), '-o', str(base/'test')], check=True)
    subprocess.run([str(base/'test')], check=True)
print('Linux sysinfo: exact layout/padding, real page accounting, byte units, '
      'uptime, overflow and metadata/user-copy errors passed')
