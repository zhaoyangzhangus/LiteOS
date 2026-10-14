"""Check actual bounded VM-lock diagnostic publication with a fake TSC."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/mm/vm_space.c').read_text()
start = source.index('typedef struct {\n    uint64_t duration_ns, root, caller, tid;')
end = source.index('\n}', source.index('static void vm_lock_record(', start)) + 2
code = source[start:end]
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdatomic.h>
static struct { uint64_t tsc_hz; } x86_boot_cpu_features = {1000000000};
static unsigned conversions;
static uint64_t x86_tsc_to_ns(uint64_t ticks) { ++conversions; return ticks; }
''' + code + r'''
int main(void) {
    vm_lock_record(0,9999999,4096,42,123);
    assert(!conversions && !g_vm_lock_diagnostics[0].duration_ns);
    vm_lock_record(0,10000000,4096,42,123);
    assert(conversions==1 && g_vm_lock_diagnostics[0].duration_ns==10000000);
    vm_lock_record(0,40000000,8192,43,124);
    assert(g_vm_lock_diagnostics[0].duration_ns==40000000);
    assert(g_vm_lock_diagnostics[0].root==8192 && g_vm_lock_diagnostics[0].tid==124);
    vm_lock_record(0,30000000,16384,44,125);
    assert(g_vm_lock_diagnostics[0].caller==43);
    atomic_flag_test_and_set(&g_vm_lock_diagnostic_guard);
    vm_lock_record(1,50000000,8192,45,126);
    assert(!g_vm_lock_diagnostics[1].duration_ns);
    assert(atomic_flag_test_and_set(&g_vm_lock_diagnostic_guard));
    atomic_flag_clear(&g_vm_lock_diagnostic_guard);
    vm_lock_record(1,50000000,8192,45,126);
    assert(g_vm_lock_diagnostics[1].duration_ns==50000000);
    assert(g_vm_lock_diagnostics[1].caller==45 && g_vm_lock_diagnostics[1].tid==126);
    assert(!atomic_flag_test_and_set(&g_vm_lock_diagnostic_guard));
    atomic_flag_clear(&g_vm_lock_diagnostic_guard);
    g_vm_lock_diagnostics[0].duration_ns=0;
    g_vm_lock_trace_threshold_ns=1000000;
    vm_lock_record(0,999999,8192,99,9);
    assert(!g_vm_lock_diagnostics[0].duration_ns);
    vm_lock_record(0,1000000,8192,99,9);
    assert(g_vm_lock_diagnostics[0].duration_ns==1000000);
    assert(g_vm_lock_diagnostics[0].caller==99);
    g_vm_lock_diagnostics[0].duration_ns=0;
    g_vm_lock_trace_threshold_ns=0;
    vm_lock_record(0,1000000,8192,99,9);
    assert(!g_vm_lock_diagnostics[0].duration_ns);
    g_vm_lock_trace_threshold_ns=1;
    vm_lock_record(0,99999,8192,99,9);
    assert(!g_vm_lock_diagnostics[0].duration_ns);
    vm_lock_record(0,100000,8192,99,9);
    assert(g_vm_lock_diagnostics[0].duration_ns==100000);
    g_vm_lock_trace_threshold_ns=UINT64_MAX;
    vm_lock_record(0,999999999,8192,99,9);
    assert(g_vm_lock_diagnostics[0].duration_ns==100000);
    vm_lock_record(0,1000000000,8192,99,9);
    assert(g_vm_lock_diagnostics[0].duration_ns==1000000000);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-vm-lock-diagnostic-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('VM lock diagnostics: threshold, worst wait/hold, owner identity and non-blocking publication passed')
