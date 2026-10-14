"""Exercise the actual wait diagnostic without changing scheduling semantics."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/sync/wait.c').read_text()
begin = source.index('typedef struct wait_delay_diagnostic')
end = source.index('\n}', source.index('static void wait_record_delay', begin)) + 2
program = r'''
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
typedef struct { uint64_t tid; } thread_t;
typedef struct {
    thread_t *thread;
    void *queue;
    uint64_t diagnostic_started_tsc, diagnostic_claimed_tsc;
} waiter_t;
static uint64_t x86_tsc_to_ns(uint64_t ticks) { return ticks; }
''' + source[begin:end] + r'''
int main(void) {
    thread_t thread = {126};
    waiter_t waiter = {&thread, (void*)0x1000, 100, 1000100};
    wait_record_delay(&waiter, 20000100);
    assert(!g_wait_delays[0].tid);
    g_wait_delay_tid = 126;
    wait_record_delay(&waiter, 9999999);
    assert(!g_wait_delays[0].tid);
    wait_record_delay(&waiter, 20000100);
    assert(g_wait_delays[0].total_ns == 20000000);
    assert(g_wait_delays[0].before_claim_ns == 1000000);
    assert(g_wait_delays[0].after_claim_ns == 19000000);
    wait_record_delay(&waiter, 10000100);
    assert(g_wait_delays[0].total_ns == 20000000);
    waiter.diagnostic_claimed_tsc = 0;
    wait_record_delay(&waiter, 30000100);
    assert(g_wait_delays[0].total_ns == 20000000);
    waiter.diagnostic_claimed_tsc = 1000100;
    for (unsigned i=1;i<32;++i) {
        waiter.queue=(void*)(uintptr_t)(0x1000+i*4096);
        wait_record_delay(&waiter, 20000100);
    }
    waiter.queue=(void*)0x100000;
    wait_record_delay(&waiter, 30000100);
    assert(g_wait_delays[31].queue == 0x20000);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-wait-delay-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(path/'test.c'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True)
print('wait delay: selector, threshold, phase split, unclaimed wake, maximum and capacity passed')
