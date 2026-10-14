"""Execute real mmap argument validation and fd-width normalization."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root/'kernel/syscall/linux.c').read_text()
start = source.index('static int64_t linux_mmap(')
end = source.index('    if ((flags & LINUX_MAP_ANONYMOUS) == 0U)', start)
function = source[start:end] + r'''
(void)object;(void)entry;(void)address;(void)status;(void)vm_prot;(void)vm_flags;
return descriptor;
}
'''
program = r'''
#include <assert.h>
#include <kernel/vm.h>
#include <uapi/linux_abi.h>
#define X86_64_USER_TOP 0x7fffffffffffULL
#define LINUX_MAP_32BIT 0x40U
#define LINUX_MAP_NORESERVE 0x4000U
typedef int process_t;
typedef struct {int dummy;} linux_fd_entry_t;
static bool x86_is_canonical(uint64_t a){return a<=X86_64_USER_TOP;}
''' + function + r'''
int main(void) {
 uint64_t high[]={0,1,45,0x7fffffff,0xffffffff};
 for(unsigned i=0;i<sizeof(high)/sizeof(high[0]);++i) {
  uint64_t raw=(high[i]<<32)|44;
  assert(linux_mmap(0,0,0x40000,3,LINUX_MAP_SHARED,(int64_t)raw,0)==44);
  raw=(high[i]<<32)|0xffffffff;
  assert(linux_mmap(0,0,4096,3,LINUX_MAP_PRIVATE,(int64_t)raw,0)==-1);
 }
 assert(linux_mmap(0,0,0,3,LINUX_MAP_SHARED,44,0)==K_EINVAL);
 assert(linux_mmap(0,0,4096,3,LINUX_MAP_SHARED,44,1)==K_EINVAL);
}
'''
with tempfile.TemporaryDirectory(prefix='linux-mmap-fd-') as directory:
    base = Path(directory)
    (base/'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                    '-I'+str(root/'include'), str(base/'test.c'), '-o', str(base/'test')], check=True)
    subprocess.run([str(base/'test')], check=True)
print('Linux mmap fd ABI: packed/high-bit registers preserve fd 44, '
      'negative descriptors remain negative, length/offset checks preserved')
