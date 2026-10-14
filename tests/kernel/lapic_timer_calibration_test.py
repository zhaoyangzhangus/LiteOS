"""Exercise production APIC calibration and BSP/AP programming with fake clocks."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/arch/x86_64/irq/apic.c').read_text()
def function(name):
    start = source.index(name + '(')
    start = source.rfind('\n', 0, start) + 1
    first = source.index('{', start)
    depth, end = 1, first + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

constants = '\n'.join(line for line in source.splitlines()
                      if line.startswith('#define LAPIC_') or
                      line.startswith('#define IA32_APIC_'))
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
typedef uint8_t UINT8, BOOLEAN;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
''' + constants + r'''
static BOOLEAN g_initialized;
static UINT32 g_timer_initial_count;
static UINT64 g_tick_count;
static atomic_bool g_timer_maintenance_pending, g_timer_maintenance_queued;
static uint32_t regs[256], delta;
static uint64_t window, step, now;
static unsigned reads, clock_calls;
static bool backwards;
static uint64_t x86_timeout_ns_to_tsc(uint64_t ns) { (void)ns; return window; }
static uint64_t x86_tsc_to_ns(uint64_t tsc) { return tsc; }
static uint64_t x86_read_tsc(void) {
    ++clock_calls;
    if (backwards && clock_calls > 1) return 1;
    uint64_t result = now;
    now += step;
    return result;
}
static void lapic_write(UINT32 reg, UINT32 value) { regs[reg/4] = value; }
static UINT32 lapic_read(UINT32 reg) {
    if (reg == LAPIC_VERSION_REGISTER) return 1;
    if (reg == LAPIC_ID_REGISTER) return 2U << 24;
    if (reg == LAPIC_TIMER_CURRENT_REGISTER) {
        assert(regs[LAPIC_LVT_TIMER_REGISTER/4] & LAPIC_TIMER_MASKED_BIT);
        assert(!(regs[LAPIC_LVT_TIMER_REGISTER/4] & LAPIC_TIMER_PERIODIC_BIT));
        return reads++ == 0 ? UINT32_MAX : UINT32_MAX - delta;
    }
    return regs[reg/4];
}
static UINT64 lapic_read_msr(UINT32 index) { (void)index; return 0; }
static void lapic_write_msr(UINT32 index, UINT64 value) { (void)index; (void)value; }
static void reset(uint32_t ticks, uint64_t elapsed) {
    delta = ticks; step = elapsed; window = 5000000;
    reads = clock_calls = 0; now = 100; backwards = false;
}
''' + function('lapic_calibrate_timer') + '\n' + function('lapic_init') + '\n' + function('lapic_init_secondary') + r'''
int main(void) {
    reset(312500, 5000000);
    assert(lapic_calibrate_timer(32, 1000000) == 125000); /* QEMU: 16ms -> 2ms */
    reset(78125, 5000000);
    assert(lapic_calibrate_timer(32, 1000000) == 31250); /* Slower APIC clock */
    reset(1000000, 20000000);
    assert(lapic_calibrate_timer(32, 1000000) == 100000); /* Actual elapsed, not window */
    reset(UINT32_MAX, 5000000);
    assert(lapic_calibrate_timer(32, 1000000) == 1717986918U); /* No multiply overflow */
    reset(0, 5000000);
    assert(lapic_calibrate_timer(32, 1000000) == 1000000);
    reset(1, 5000000);
    assert(lapic_calibrate_timer(32, 1000000) == 1000000); /* Rounded to zero */
    reset(312500, 5000000); window = 0;
    assert(lapic_calibrate_timer(32, 1000000) == 1000000 && clock_calls == 0);
    reset(312500, 0);
    assert(lapic_calibrate_timer(32, 1000000) == 1000000);
    assert(clock_calls == 1000001); /* Stuck TSC must not hang boot */
    reset(312500, 5000000); backwards = true;
    assert(lapic_calibrate_timer(32, 1000000) == 1000000);
    reset(312500, 100000001);
    assert(lapic_calibrate_timer(32, 1000000) == 1000000);
    reset(312500, 5000000);
    assert(!lapic_init(31, 1000000));
    assert(!lapic_init(32, 0));
    assert(lapic_init(32, 1000000));
    assert(g_timer_initial_count == 125000 && g_tick_count == 0);
    assert(regs[LAPIC_TIMER_INITIAL_REGISTER/4] == 125000);
    assert(regs[LAPIC_LVT_TIMER_REGISTER/4] == (32U | LAPIC_TIMER_PERIODIC_BIT));
    unsigned calibrated_calls = clock_calls;
    assert(!lapic_init(32, 1000000));
    assert(!lapic_init_secondary(3, 32, 1000000));
    assert(lapic_init_secondary(2, 32, 1000000));
    assert(regs[LAPIC_TIMER_INITIAL_REGISTER/4] == 125000);
    assert(regs[LAPIC_LVT_TIMER_REGISTER/4] ==
           (32U | LAPIC_TIMER_PERIODIC_BIT | LAPIC_TIMER_MASKED_BIT));
    assert(clock_calls == calibrated_calls); /* AP inherits BSP calibration */
    puts("APIC timer: measured period, bounded failure fallback, arithmetic, masked calibration and BSP/AP consistency passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-apic-calibration-') as directory:
    path = Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(path/'test.c'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True)
