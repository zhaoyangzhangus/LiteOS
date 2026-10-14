"""Exercise the actual performance owner and benchmark schema parser."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
assert 'LITEOS_' not in (ROOT / 'kernel/perf.c').read_text()
code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define printf test_console_printf
''' + f'#include "{ROOT / "kernel/console_printf.c"}"\n' + r'''
#undef printf
''' + f'#include "{ROOT / "kernel/perf.c"}"\n' + r'''
static char output[1024], memory[64];
static unsigned allocations, frees, clock_calls, records;
static unsigned fail_at=UINT32_MAX;
static bool fixed_clock;
static uint64_t fixed_timestamp;
void *kmalloc(size_t size, uint32_t flags) {
    assert(size == 64 && !flags);
    return allocations++ == fail_at ? NULL : memory;
}
void kfree(void *pointer) { assert(pointer == memory); ++frees; }
uint64_t kmem_fastpath_hits(void) { return frees; }
uint64_t kmem_fastpath_refills(void) { return 0; }
uint64_t telemetry_timestamp(void) {
    if (fixed_clock) return fixed_timestamp;
    unsigned call=clock_calls++;
    return (call/2)*1000 + (call%2 ? 128-call/2 : 0);
}
void kernel_serial_write(const char *text) { (void)text; assert(0); }
void kernel_serial_write_serial_only(const char *text) {
    assert(strlen(text)<sizeof(output)); strcpy(output,text); ++records;
}
int main(void) {
    kernel_perf_report_t report;
    assert(!kernel_perf_benchmark(NULL) && !allocations);
    assert(kernel_perf_benchmark(&report));
    assert(allocations == 160 && frees == 160 && clock_calls == 256);
    assert(report.samples == 128 && report.min_tsc == 1 && report.max_tsc == 128);
    assert(report.median_tsc == 65 && report.p95_tsc == 122 && report.average_tsc == 64);
    assert(report.fastpath_hits == 128 && !report.fastpath_refills);
    kernel_perf_emit_report_at(&report,"kernel/perf.c",42);
    assert(!strcmp(output,"OS_PERF_KMALLOC_OK SAMPLES=128 MIN=1 MEDIAN=65 P95=122 AVG=64 MAX=128 HITS=128 REFILLS=0 loc=kernel/perf.c:42\r\n"));
    fixed_clock=true; fixed_timestamp=0;
    kernel_perf_emit_scope_at("wrap",1,NULL,UINT32_MAX);
    assert(!strcmp(output,"OS_BENCH name=wrap cycles=18446744073709551615 loc=<unknown>:4294967295\r\n"));
    kernel_perf_emit_value_at("maximum",UINT64_MAX,"kernel/test.c",1);
    assert(!strcmp(output,"OS_BENCH_VALUE name=maximum value=18446744073709551615 loc=kernel/test.c:1\r\n"));
    kernel_perf_emit_scope("scope",0);
    assert(!strcmp(output,"OS_BENCH name=scope cycles=0 loc=<unknown>:0\r\n"));
    kernel_perf_emit_value("value",0);
    assert(!strcmp(output,"OS_BENCH_VALUE name=value value=0 loc=<unknown>:0\r\n"));
    kernel_perf_emit_report(&report); assert(strstr(output,"loc=<unknown>:0\r\n"));
    unsigned baseline=records;
    kernel_perf_emit_scope_at(NULL,0,NULL,0);
    kernel_perf_emit_value_at(NULL,0,NULL,0);
    kernel_perf_emit_report_at(NULL,NULL,0); assert(records == baseline);
    for (unsigned failure=0; failure<160; ++failure) {
        allocations=frees=clock_calls=0; fixed_clock=false; fail_at=failure;
        memset(&report,0xa5,sizeof(report)); kernel_perf_report_t before=report;
        assert(!kernel_perf_benchmark(&report));
        assert(allocations == failure+1 && frees == failure);
        assert(!memcmp(&report,&before,sizeof(report)));
    }
    puts("PERF_DIAGNOSTIC_TEST_OK");
}
'''
with tempfile.TemporaryDirectory(prefix='perf-diagnostics-') as temporary:
    fixture = Path(temporary) / 'test.c'; binary = Path(temporary) / 'test'
    fixture.write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-fno-builtin',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I', str(ROOT / 'include'), str(fixture), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    metrics = []
    for line in (ROOT / 'tools/roadmap-benchmark-metrics.tsv').read_text().splitlines():
        if line and not line.startswith('#'):
            metrics.append(line.split('|'))
    log = Path(temporary) / 'serial.log'
    def check(profile, lines, expected_status):
        log.write_text(''.join(lines))
        result = subprocess.run(['bash', str(ROOT / 'tools/verify-benchmark-schema.sh'), profile, str(log)], capture_output=True, text=True)
        assert result.returncode == expected_status, result.stdout + result.stderr
    all_records = [f'OS_BENCH{"_VALUE" if i%2 else ""} name={name} {"value" if i%2 else "cycles"}=1 loc=kernel/perf.c:42\r\n' for i,(name,_) in enumerate(metrics)]
    common = [record for record,(_,profile) in zip(all_records,metrics) if profile != 'nvme']
    check('usb',common,0); check('nvme',all_records,0); check('auto',common,0)
    check('auto',['OS_NVME_HW_OK\r\n',*all_records],0)
    for i in range(len(common)):
        check('usb',common[:i]+common[i+1:],1)
    check('nvme',common,1); check('invalid',all_records,2)
    check('usb',[record.replace('OS_BENCH','LITE'+'OS_BENCH') for record in common],1)
    print('BENCHMARK_SCHEMA_PROTOCOL_TEST_OK')
