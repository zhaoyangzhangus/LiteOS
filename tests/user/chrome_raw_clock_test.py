"""Run Abseil's actual TSC calibration through the production Linux adapter."""
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from chromium_source import chromium_source

root = Path(__file__).resolve().parents[2]
linux = (root/'kernel/syscall/linux.c').read_text()
adapter = linux[linux.index('static int64_t linux_clock_gettime('):
                linux.index('static int64_t linux_gettimeofday(')]
absl = (chromium_source()/'third_party/abseil-cpp/absl/base/internal/sysinfo.cc').read_text()
start = absl.index('static int64_t ReadMonotonicClockNanos()')
calibration = absl[start:absl.index('\n#endif  // ABSL_INTERNAL_UNSCALED_CYCLECLOCK_FREQUENCY_IS_CPU_FREQUENCY', start)]
prefix = r'''
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <limits>
#include <stdexcept>
#define __user
#define K_OK 0
#define K_EINVAL -22
#define K_EPERM -1
#define LINUX_EFAULT -14
#define OS_LINUX_CLOCK_REALTIME 0
#define OS_LINUX_CLOCK_MONOTONIC 1
using kstatus_t=int;
struct linux_timespec_t {int64_t seconds,nanoseconds;};
using os_timespec_t=linux_timespec_t;
struct process_t {uint64_t pid;};
struct thread_t {process_t *process;struct {uint64_t runtime_ns;} sched;};
struct os_task_info_t {uint64_t runtime_ns;};
static thread_t *sched_current_thread(){return nullptr;}
static int process_task_query(uint64_t,uint64_t,os_task_info_t*){return K_EPERM;}
static int copy_to_user(void *to,const void *from,size_t size){std::memcpy(to,from,size);return 0;}
static uint64_t now_ns=1000000000;
static unsigned raw_reads=0,sleeps=0;
static int kernel_clock_read(uint32_t id,os_timespec_t *value){
 assert(id==OS_LINUX_CLOCK_MONOTONIC);
 value->seconds=now_ns/1000000000;
 value->nanoseconds=now_ns%1000000000;
 now_ns+=100;return 0;
}
''' + adapter + r'''
static int fixture_clock_gettime(int id,timespec *value){
 assert(id==CLOCK_MONOTONIC_RAW);++raw_reads;
 linux_timespec_t result;
 int status=linux_clock_gettime(id,reinterpret_cast<uintptr_t>(&result));
 if(status){errno=-status;return -1;}
 value->tv_sec=result.seconds;value->tv_nsec=result.nanoseconds;return 0;
}
static int fixture_nanosleep(const timespec *request,timespec*){
 assert(request->tv_sec==0&&request->tv_nsec>0);++sleeps;
 now_ns+=request->tv_nsec;return 0;
}
namespace base_internal {
struct UnscaledCycleClock {static int64_t Now(){return now_ns*3;}};
}
#define clock_gettime fixture_clock_gettime
#define nanosleep fixture_nanosleep
#define ABSL_RAW_LOG(...) throw std::runtime_error("clock calibration failed")
'''
suffix = r'''
int main(){
 try {
  double hz=MeasureTscFrequency();
  assert(hz>2.99e9&&hz<3.01e9);
  assert(sleeps>=2&&raw_reads>=40);
  return 0;
 } catch(const std::runtime_error&) {return 77;}
}
'''
with tempfile.TemporaryDirectory(prefix='chrome-raw-clock-') as directory:
    fixture = Path(directory)
    for label, body, expected in [
        ('actual', adapter, 0),
        ('missing-raw', adapter.replace(
            '    if (clock_id == 4U) clock_id = OS_LINUX_CLOCK_MONOTONIC;', ''), 77),
    ]:
        assert body != adapter or label == 'actual'
        program = prefix.replace(adapter, body) + calibration + suffix
        source = fixture/(label+'.cc')
        source.write_text(program)
        binary = fixture/label
        subprocess.run(['c++','-std=c++20','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-pie','-no-pie',
                        str(source),'-o',str(binary)], check=True)
        result = subprocess.run([str(binary)], timeout=10)
        assert result.returncode == expected, (label,result.returncode)
print('Actual Abseil TSC calibration + Linux clock adapter: RAW reads and '
      'frequency convergence passed; missing RAW reproduces fatal path')
