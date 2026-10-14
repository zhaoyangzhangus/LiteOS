"""Execute actual clear_child_tid cleanup; pthread_join needs a shared wake."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/syscall/linux.c').read_text()
begin = source.index('void linux_thread_exit_cleanup(')
end = source.index('\nbool linux_process_is(', begin)
code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#define __user
#define K_OK 0
typedef uint64_t vaddr_t;
typedef struct process { int identity; } process_t;
typedef struct thread {
    bool linux_clear_tid_pending;
    vaddr_t linux_clear_tid;
    process_t *process;
} thread_t;
static int stores, wakes, fault;
static process_t process;
static uint32_t tid=131;
static int put_user_u32(uint32_t *address, uint32_t value) {
    assert(address==&tid && value==0); ++stores;
    if (fault) return -14;
    *address=value; return 0;
}
static int futex_wake_keyed(process_t *p, uint32_t *address,
                             uint32_t max, uint32_t *count, bool private_key) {
    assert(p==&process && address==&tid && !tid && max==1 && !private_key);
    ++wakes; *count=1; return 0;
}
''' + source[begin:end] + r'''
int main(void) {
    thread_t t={true,(uintptr_t)&tid,&process};
    linux_thread_exit_cleanup(0);
    linux_thread_exit_cleanup(&t);
    assert(!tid && !t.linux_clear_tid_pending && stores==1 && wakes==1);
    linux_thread_exit_cleanup(&t); assert(stores==1 && wakes==1);
    t.linux_clear_tid_pending=true; tid=132; fault=1;
    linux_thread_exit_cleanup(&t);
    assert(tid==132 && stores==2 && wakes==1 && !t.linux_clear_tid_pending);
    t.linux_clear_tid_pending=true; t.linux_clear_tid=0;
    linux_thread_exit_cleanup(&t); assert(stores==2 && wakes==1);
}
'''
with tempfile.TemporaryDirectory(prefix='linux-clear-tid-') as directory:
    base = Path(directory)
    (base/'test.c').write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(base/'test.c'),
                    '-o',str(base/'test')],check=True)
    subprocess.run([str(base/'test')],check=True)
print('Linux clear_child_tid: real cleanup, clear-before-shared-wake, exactly-once, null and write faults passed')
