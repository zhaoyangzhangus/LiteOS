"""Run the production allocator initialization gate with injected failures."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
for path in (ROOT / 'kernel/init').glob('*.c'):
    assert 'LITEOS_' not in path.read_text(), path
markers = set()
for path in (ROOT / 'kernel/init').glob('*.c'):
    for literal in re.findall(r'"(?:\\.|[^"\\])*"', path.read_text()):
        markers.update(re.findall(r'OS_[A-Z0-9_]+', literal))
legacy = {'LITE' + 'OS_' + marker[3:] for marker in markers}
for directory in ('kernel', 'include', 'user', 'tools', 'tests', 'boot'):
    for path in (ROOT / directory).rglob('*'):
        if path.suffix not in ('.c', '.h', '.S', '.asm', '.py', '.sh', '.ps1', '.mk', '.tsv'):
            continue
        remaining = set(re.findall(r'LITE' + r'OS_[A-Z0-9_]+', path.read_text())) & legacy
        assert not remaining, (path, remaining)
verifier = (ROOT / 'tools/verify-qemu-stages.sh').read_text()
assert 'LITEOS_' not in verifier
code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
''' + f'#include "{ROOT / "kernel/init/core.c"}"\n' + r'''
static unsigned injected, calls, halts, failures, enters, ready, progress;
static char output[1024];
static bool test(unsigned stage) { ++calls; return injected != stage; }
bool kmem_self_test(void) { return test(1); }
bool kmem_stress_self_test(void) { return test(2); }
uint64_t kmem_fastpath_hits(void) { ++calls; return injected == 3 ? 0 : 1; }
uint32_t x86_current_cpu_index(void) { return 0; }
bool vmalloc_self_test(void) { return test(4); }
bool vmalloc_tlb_reuse_self_test(void) { return test(5); }
void realtest_mark(const char *state) { assert(state); }
void kernel_debug_stage_at(uint16_t phase, uint16_t step, uint32_t value,
                           const char *file, uint32_t line) {
    assert(strstr(file,"kernel/init/core.c") && line);
    if (step == OS_DEBUG_STEP_ENTER) {
        assert(phase == OS_DEBUG_PHASE_SPEC_3 && !value); ++enters;
    } else if (step == OS_DEBUG_STEP_READY) {
        assert(phase == OS_DEBUG_PHASE_SPEC_3 && value == 1); ++ready;
    } else {
        assert(step == OS_DEBUG_STEP_PROGRESS && phase == OS_DEBUG_PHASE_REFACTOR_3 && value == 4); ++progress;
    }
}
void kernel_debug_stage_fail_at(uint16_t phase, uint16_t step, int64_t status,
                                const char *file, uint32_t line) {
    assert(phase == OS_DEBUG_PHASE_SPEC_3 && step == OS_DEBUG_STEP_FAIL && status == K_EIO);
    assert(strstr(file,"kernel/init/core.c") && line); ++failures;
}
static void write_record(const CHAR8 *text) {
    assert(strlen(output) + strlen(text) < sizeof(output)); strcat(output,text);
}
static void halt(void) { ++halts; }
int main(void) {
    kernel_init_core_hooks_t hooks = {write_record,halt};
    assert(!kernel_init_core(NULL));
    kernel_init_core_hooks_t invalid = {0}; assert(!kernel_init_core(&invalid));
    invalid.write = write_record; assert(!kernel_init_core(&invalid));
    assert(!enters && !calls && !halts);
    const char *failed[] = {"", "OS_KMALLOC_TEST_FAIL\r\n", "OS_KMALLOC_STRESS_FAIL\r\n",
        "OS_KMALLOC_PERCPU_FAIL\r\n", "OS_VMALLOC_TEST_FAIL\r\n", "OS_TLB_REUSE_FAIL\r\n"};
    for (injected = 0; injected <= 5; ++injected) {
        output[0] = 0; calls = halts = failures = enters = ready = progress = 0;
        assert(kernel_init_core(&hooks) == (injected == 0));
        assert(enters == 1 && calls == (injected ? injected : 5));
        assert(halts == (injected != 0) && failures == halts);
        assert(ready == (injected == 0) && progress == ready);
        if (injected) {
            size_t n = strlen(output), m = strlen(failed[injected]);
            assert(n >= m && !strcmp(output + n - m,failed[injected]));
        } else {
            assert(!strcmp(output,"OS_KMALLOC_OK\r\nOS_KMALLOC_STRESS_OK\r\nOS_KMALLOC_PERCPU_OK\r\nOS_VMALLOC_OK\r\nOS_TLB_REUSE_OK\r\n"));
        }
    }
    puts("INIT_CORE_DIAGNOSTIC_TEST_OK");
}
'''
with tempfile.TemporaryDirectory(prefix='init-core-diagnostics-') as tmp:
    fixture = Path(tmp) / 'test.c'; binary = Path(tmp) / 'test'
    fixture.write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I', str(ROOT / 'include'), str(fixture), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
