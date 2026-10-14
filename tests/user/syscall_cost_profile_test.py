"""Test the actual bounded opt-in syscall cost accumulator."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/syscall/handlers.c').read_text()
start = source.index('typedef struct syscall_cost_diagnostic')
end = source.index('static bool syscall_measure_linux_delay', start)
production = source[start:end]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
static uint64_t x86_tsc_to_ns(uint64_t n) { return n; }
''' + production + r'''
int main(void) {
    syscall_record_cost(1, 0, 3);
    assert(g_syscall_costs[0].count==0);
    g_syscall_cost_tid=42;
    syscall_record_cost(41, 0, 3);
    assert(g_syscall_costs[0].count==0);
    syscall_record_cost(42, 0, 3);
    syscall_record_cost(42, 0, 8);
    assert(g_syscall_costs[0].count==2);
    assert(g_syscall_costs[0].total_ns==11);
    assert(g_syscall_costs[0].maximum_ns==8);
    syscall_record_cost(42, UINT64_C(0x8000000000000006), 5);
    assert(g_syscall_costs[1].number==UINT64_C(0x8000000000000006));
    atomic_flag_test_and_set(&g_syscall_cost_guard);
    syscall_record_cost(42, 0, 20);
    assert(g_syscall_cost_dropped==1 && g_syscall_costs[0].count==2);
    atomic_flag_clear(&g_syscall_cost_guard);
    for (unsigned i=1;i<128;i++) syscall_record_cost(42,i,1);
    syscall_record_cost(42, 999, 9);
    assert(g_syscall_cost_dropped==3);
    g_syscall_cost_tid=0;
    syscall_record_cost(42, 0, 99);
    assert(g_syscall_costs[0].count==2);
}
'''
with tempfile.TemporaryDirectory(prefix='system-syscall-cost-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('Opt-in TID, syscall zero/native identity, cumulative/max costs, bounded drops and disable passed')
