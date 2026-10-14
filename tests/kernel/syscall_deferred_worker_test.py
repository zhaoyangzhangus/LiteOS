"""The actual syscall policy must not run arbitrary callbacks after boot."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'kernel/syscall/handlers.c').read_text()
policy=source[source.index('    if (!deferred_worker_started())'):]
policy=policy[:policy.index('\n')]
code=r'''
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
static bool started;
static unsigned callbacks;
static bool deferred_worker_started(void) { return started; }
static uint32_t deferred_run(uint32_t budget) { callbacks+=budget;return budget; }
static void syscall_work(void) {
''' + policy + r'''
}
int main(void) {
 syscall_work();assert(callbacks==4);
 started=true;
 for(unsigned i=0;i<10000;++i)syscall_work();
 assert(callbacks==4);
 started=false;syscall_work();assert(callbacks==8);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-syscall-worker-') as directory:
 binary=str(Path(directory)/'test')
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                 '-fsanitize=address,undefined','-fno-pie','-no-pie',
                 '-x','c','-','-o',binary],input=code,text=True,check=True)
 subprocess.run([binary],check=True)
print('Syscalls: bootstrap drain retained; 10000 post-worker calls execute no global callbacks')
