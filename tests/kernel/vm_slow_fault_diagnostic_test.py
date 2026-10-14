"""Production slow-fault recorder: leaders do not mask one another."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/mm/fault.c').read_text()
begin = source.index('typedef struct vm_slow_fault_diagnostic')
end = source.index('\n}', source.index('static void vm_record_slow_fault(', begin)) + 2
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
static struct { uint64_t tsc_hz; } x86_boot_cpu_features={1000000000ULL};
static uint64_t x86_tsc_to_ns(uint64_t ticks) { return ticks; }
''' + source[begin:end] + r'''
int main(void) {
    vm_record_slow_fault(125,126,0x4000,7,0,9999999,9999999);
    assert(!g_vm_slow_faults[0].tid);
    vm_record_slow_fault(125,126,0x4000,7,0,2000000,10000000);
    assert(g_vm_slow_faults[0].pid==125 && g_vm_slow_faults[0].tid==126);
    assert(g_vm_slow_faults[0].fault_ns==2000000 && g_vm_slow_faults[0].around_ns==8000000);
    vm_record_slow_fault(125,126,0x8000,5,0,20000000,30000000);
    assert(g_vm_slow_faults[0].address==0x8000 && g_vm_slow_faults[0].error==5);
    vm_record_slow_fault(125,126,0xc000,1,0,10000000,10000000);
    assert(g_vm_slow_faults[0].total_ns==30000000);
    assert(!atomic_flag_test_and_set(&g_vm_slow_fault_guard));
    vm_record_slow_fault(125,126,0xc000,1,0,100000000,100000000);
    assert(g_vm_slow_faults[0].total_ns==30000000);
    assert(atomic_flag_test_and_set(&g_vm_slow_fault_guard));
    atomic_flag_clear(&g_vm_slow_fault_guard);
    for (unsigned i=1;i<32;++i) vm_record_slow_fault(i,i,0x1000,0,0,20000000,20000000);
    vm_record_slow_fault(999,999,0x1000,0,0,300000000,300000000);
    for (unsigned i=1;i<32;++i) assert(g_vm_slow_faults[i].tid==i);
    assert(g_vm_slow_faults[0].tid==126);
    x86_boot_cpu_features.tsc_hz=0;
    vm_record_slow_fault(125,126,0xc000,1,0,100000000,100000000);
    assert(g_vm_slow_faults[0].total_ns==30000000);
    puts("slow fault recorder: split durations, per-leader maxima, bounded capacity and nonblocking publication passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-slow-fault-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
