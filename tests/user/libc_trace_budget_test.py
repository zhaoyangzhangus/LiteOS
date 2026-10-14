"""Exercise the real saturating quota at each file-diagnostic limit."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'user/libc/syscall.c').read_text()
helper = source[source.index('static bool libc_trace_budget('):source.index('static void libc_debug_text(')]
assert '__atomic_fetch_add' not in source
for call in ('&calls, 64U', '&g_libc_close_fail_debug_calls, 64U',
             '&g_libc_profile_open_debug_calls, 256U',
             '&g_libc_database_open_debug_calls, 128U',
             '&count, 16U', '&g_libc_fcntl_lock_debug_calls, 64U',
             'counter, 128U'):
    assert 'libc_trace_budget(' + call + ')' in source

program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
static volatile uint32_t counter;
static uint32_t quota;
static atomic_uint admitted, exchanges;
static bool checked_exchange(volatile uint32_t *p, uint32_t *expected,
                             uint32_t desired, bool weak, int success, int failure) {
    atomic_fetch_add(&exchanges, 1);
    return __atomic_compare_exchange_n(p, expected, desired, weak, success, failure);
}
#define __atomic_compare_exchange_n checked_exchange
''' + helper + r'''
#undef __atomic_compare_exchange_n
static void *worker(void *unused) {
    (void)unused;
    for (unsigned i=0; i<10000; ++i)
        if (libc_trace_budget(&counter, quota)) atomic_fetch_add(&admitted, 1);
    return 0;
}
int main(void) {
    const uint32_t limits[] = {0, 1, 16, 64, 128, 256};
    for (unsigned i=0; i<sizeof(limits)/sizeof(*limits); ++i) {
        quota=limits[i]; counter=0; atomic_store(&admitted,0);
        pthread_t threads[8];
        for (unsigned n=0; n<8; ++n) assert(!pthread_create(&threads[n],0,worker,0));
        for (unsigned n=0; n<8; ++n) assert(!pthread_join(threads[n],0));
        assert(counter==quota && atomic_load(&admitted)==quota);
        atomic_store(&exchanges,0);
        for (unsigned n=0; n<10000; ++n) assert(!libc_trace_budget(&counter,quota));
        assert(!atomic_load(&exchanges) && counter==quota);
        counter=UINT32_MAX;
        assert(!libc_trace_budget(&counter,quota) && counter==UINT32_MAX);
    }
    counter=UINT32_MAX-1;
    assert(libc_trace_budget(&counter,UINT32_MAX) && counter==UINT32_MAX);
    atomic_store(&exchanges,0);
    assert(!libc_trace_budget(&counter,UINT32_MAX));
    assert(counter==UINT32_MAX && !atomic_load(&exchanges));
}
'''
with tempfile.TemporaryDirectory(prefix='libc-trace-budget-') as directory:
    path=Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('File trace budgets: seven consumers, exact concurrent limits, overflow and zero exhausted exchanges passed')
