"""Exercise production normal/signal exit glue and first-exit ownership."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/process/exit.c').read_text()
functions = source[source.index('static __noreturn void process_exit_internal('):
                   source.index('__noreturn void thread_exit(')]
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <setjmp.h>
#define __noreturn _Noreturn
typedef struct { unsigned state; } process_t;
typedef struct { process_t *process; } thread_t;
static process_t process;
static thread_t current = { &process };
static thread_t *running;
static jmp_buf escape;
static unsigned transitions, cascades, diagnostics, exits;
static int64_t expected_status;
static uint32_t expected_signal;
static thread_t *sched_current_thread(void) { return running; }
static bool process_begin_exit(process_t *p, int64_t status, uint32_t signal) {
    assert(p == &process && status == expected_status && signal == expected_signal);
    ++transitions;
    if (p->state) return false;
    p->state = 1;
    return true;
}
static void process_exit_diag(process_t *p, thread_t *t, int64_t status, uint32_t signal) {
    assert(p == &process && t == &current && status == expected_status && signal == expected_signal);
    ++diagnostics;
}
static void process_terminate_siblings(process_t *p, thread_t *exclude, int64_t status) {
    assert(p == &process && p->state == 1 && exclude == running && status == expected_status);
    ++cascades;
}
static __noreturn void thread_exit(int64_t status) {
    assert(status == expected_status);
    ++exits;
    longjmp(escape, 1);
}
''' + functions + r'''
int main(void) {
    running = &current;
    expected_status = 42;
    if (!setjmp(escape)) process_exit(42);
    assert(transitions == 1 && cascades == 1 && diagnostics == 1 && exits == 1);
    if (!setjmp(escape)) process_exit(42);
    assert(transitions == 2 && cascades == 1 && diagnostics == 1 && exits == 2);
    process.state = 0; expected_status = -9; expected_signal = 9;
    if (!setjmp(escape)) process_exit_signal(9);
    assert(transitions == 3 && cascades == 2 && diagnostics == 2 && exits == 3);
    running = 0;
    if (!setjmp(escape)) process_exit_signal(9);
    assert(transitions == 3 && cascades == 2 && exits == 4);
    running = &current; current.process = 0;
    if (!setjmp(escape)) process_exit_signal(9);
    assert(transitions == 3 && cascades == 2 && exits == 5);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-exit-cascade-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('Process exit: normal/signal status, current-thread exclusion, single cascade owner and absent current process passed')
