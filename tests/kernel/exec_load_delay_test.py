"""Exercise the actual bounded exec phase recorder without a guest."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/process/exec.c').read_text()
begin = source.index('typedef struct exec_load_delay')
end = source.index('\nstatic uint64_t exec_load_start', begin)
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
static struct { uint64_t tsc_hz; } x86_boot_cpu_features = {1000000000};
static uint64_t x86_tsc_to_ns(uint64_t value) { return value; }
''' + source[begin:end] + r'''
int main(void) {
    assert(g_exec_load_timing_enabled == 0);
    exec_load_record(125,126,1,1024,0,1999999);
    assert(g_exec_load_delays[0].tid == 0);
    exec_load_record(125,126,1,1024,0,2000000);
    exec_load_record(125,126,1,2048,-5,4000000);
    exec_load_record(125,126,1,4096,0,3000000);
    assert(g_exec_load_delays[0].duration_ns == 4000000);
    assert(g_exec_load_delays[0].bytes == 2048);
    assert(g_exec_load_delays[0].status == -5);
    exec_load_record(125,126,2,1024,0,2000000);
    assert(g_exec_load_delays[1].stage == 2);
    exec_load_record(126,126,1,1024,0,2000000);
    assert(g_exec_load_delays[2].pid == 126);
    atomic_flag_test_and_set(&g_exec_load_delay_guard);
    exec_load_record(125,126,1,1024,0,9000000);
    assert(g_exec_load_delays[0].duration_ns == 4000000);
    assert(atomic_flag_test_and_set(&g_exec_load_delay_guard));
    atomic_flag_clear(&g_exec_load_delay_guard);
    for (unsigned i=3;i<64;++i) exec_load_record(i,i,1,1024,0,2000000);
    exec_load_record(999,999,1,1024,0,2000000);
    assert(g_exec_load_delays[63].tid == 63);
    x86_boot_cpu_features.tsc_hz=0;
    exec_load_record(125,126,1,1024,0,9000000);
    assert(g_exec_load_delays[0].duration_ns == 4000000);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-exec-load-delay-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('Exec phase recorder: threshold, identity, maxima, capacity and nonblocking guard passed')
