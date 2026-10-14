"""Execute the actual Linux wall-clock adapter without touching the host clock."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/syscall/linux.c').read_text()
start = source.index('static int64_t linux_gettimeofday(')
end = source.index('\nstatic int64_t linux_uname(', start)
assert '#define LINUX_SYS_GETTIMEOFDAY 96U' in source
assert 'case LINUX_SYS_GETTIMEOFDAY:\n        return linux_gettimeofday(frame->rdi, frame->rsi);' in source
fixture = r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#define __user
#define K_OK 0
#define LINUX_EFAULT -14
#define OS_CLOCK_REALTIME 0
typedef int kstatus_t;
typedef struct { int64_t seconds, nanoseconds; } os_timespec_t;
static int reads, clock_error;
static int64_t seconds = 1720000000, nanoseconds = 999999999;
static kstatus_t kernel_clock_read(uint32_t id, os_timespec_t *value) {
    assert(id == OS_CLOCK_REALTIME); ++reads;
    value->seconds=seconds; value->nanoseconds=nanoseconds;
    return clock_error;
}
static kstatus_t copy_to_user(void *to, const void *from, size_t n) {
    if ((uintptr_t)to == 1) return -14;
    memcpy(to, from, n); return 0;
}
''' + source[start:end] + r'''
int main(void) {
    int64_t tv[3]={0,0,77}; int32_t tz[3]={5,6,88};
    assert(!linux_gettimeofday((uintptr_t)tv,(uintptr_t)tz));
    assert(tv[0]==seconds && tv[1]==999999 && tv[2]==77);
    assert(tz[0]==0 && tz[1]==0 && tz[2]==88);
    ++seconds; nanoseconds=1001;
    assert(!linux_gettimeofday((uintptr_t)tv,0));
    assert(tv[0]==seconds && tv[1]==1);
    int old=reads; assert(!linux_gettimeofday(0,0) && reads==old);
    assert(!linux_gettimeofday(0,(uintptr_t)tz) && reads==old);
    assert(linux_gettimeofday(1,0)==-14);
    assert(linux_gettimeofday(0,1)==-14);
    clock_error=-5;
    assert(linux_gettimeofday((uintptr_t)tv,0)==-5);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='linux-timeval-') as directory:
    base = Path(directory)
    (base/'test.c').write_text(fixture)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(base/'test.c'),
                    '-o',str(base/'test')],check=True)
    subprocess.run([str(base/'test')],check=True)
print('Linux gettimeofday: real adapter, advancing clock, microseconds, UTC timezone, null pointers and faults passed')
