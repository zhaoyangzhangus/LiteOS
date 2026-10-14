"""Run the production stage logger with serial/capture stubs under contention."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'kernel/debug_stage.c').read_text()
assert 'LITEOS_STAGE' not in source and '"large"' not in source
consumers = ('refactor-benchmark.sh', 'run-qemu-native-matrix.ps1',
             'test_xhci_hub_hotplug.py', 'test_xhci_hub_hotplug_race.py',
             'test_xhci_hub_wakeup_probe.py', 'verify-qemu-stages.sh',
             'verify-roadmap-stages.sh')
for name in consumers:
    text = (ROOT / 'tools' / name).read_text()
    assert 'OS_STAGE' in text and 'LITEOS_STAGE' not in text, name
code = r'''
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel/console.h>
''' + f'#include "{ROOT / "kernel/debug_stage.c"}"\n' + r'''
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local int owned;
static unsigned serial_count, capture_count;
static uint64_t previous;
static char last[768];
void kernel_serial_guard_enter(kernel_serial_guard_t *guard) {
    (void)guard; assert(!owned); assert(!pthread_mutex_lock(&mutex)); owned = 1;
}
void kernel_serial_guard_leave(kernel_serial_guard_t *guard) {
    (void)guard; assert(owned); owned = 0; assert(!pthread_mutex_unlock(&mutex));
}
static void inspect(const char *text) {
    assert(owned && strlen(text) < sizeof(last));
    assert(!strncmp(text, "OS_STAGE", 8));
    assert(strstr(text, " loc=kernel/fixture.c:123\r\n"));
    const char *sequence = strstr(text, " seq="); assert(sequence);
    uint64_t current = strtoull(sequence + 5, NULL, 10);
    assert(current == previous + 1); previous = current;
    strcpy(last, text);
}
void kernel_serial_write_record_guarded(const char *text) {
#if OS_DEBUG_SERIAL
    inspect(text); ++serial_count;
#else
    (void)text; assert(0);
#endif
}
void realtest_capture(const char *text) {
#if OS_REALTEST
    assert(owned); ++capture_count;
#if !OS_DEBUG_SERIAL
    inspect(text);
#else
    assert(!strncmp(text, "OS_STAGE", 8));
#endif
#else
    (void)text; assert(0);
#endif
}
static void *worker(void *unused) {
    (void)unused;
    for (unsigned i = 0; i < 1000; ++i)
        kernel_debug_stage_at(OS_DEBUG_PHASE_CPU, OS_DEBUG_STEP_PROGRESS, i,
                              "kernel/fixture.c", 123);
    return NULL;
}
int main(void) {
    atomic_store(&g_stage_sequence, UINT32_MAX); previous = UINT32_MAX;
    kernel_debug_stage_at(OS_DEBUG_PHASE_BOOT, OS_DEBUG_STEP_READY, UINT32_MAX,
                          "kernel/fixture.c", 123);
    assert(kernel_debug_stage_sequence() == UINT64_C(4294967296));
#if OS_DEBUG_SERIAL || OS_REALTEST
    assert(strstr(last, " value=4294967295 seq=4294967296 "));
    char output[64]; uint32_t length = 0; output[0] = 0;
    stage_append_u64(output, &length, UINT64_MAX);
    assert(!strcmp(output, "18446744073709551615"));
#endif
    int64_t values[] = {0, -1, INT64_MIN, INT64_MAX, INT64_C(4294967296), -INT64_C(4294967296)};
    for (unsigned i = 0; i < sizeof(values)/sizeof(values[0]); ++i) {
        kernel_debug_stage_fail_at(OS_DEBUG_PHASE_MEMORY, OS_DEBUG_STEP_FAIL,
                                   values[i], "kernel/fixture.c", 123);
        assert(kernel_debug_stage_status() == values[i]);
#if OS_DEBUG_SERIAL || OS_REALTEST
        char expected[80]; snprintf(expected, sizeof(expected), " status=%lld seq=", (long long)values[i]);
        assert(strstr(last, expected) && !strncmp(last, "OS_STAGE_FAIL phase=MEMORY", 26));
#endif
    }
    uint64_t count = kernel_debug_stage_sequence();
    debug_trace_set(false);
    debug_trace_stage_at(OS_DEBUG_PHASE_CPU, 5, 1, "kernel/fixture.c", 123);
    assert(kernel_debug_stage_sequence() == count);
    debug_trace_set(true);
    debug_trace_stage_at(OS_DEBUG_PHASE_CPU, 5, 1, "kernel/fixture.c", 123);
    pthread_t threads[4];
    for (unsigned i = 0; i < 4; ++i) assert(!pthread_create(&threads[i], NULL, worker, NULL));
    for (unsigned i = 0; i < 4; ++i) assert(!pthread_join(threads[i], NULL));
    assert(kernel_debug_stage_sequence() == count + 4001);
    assert(serial_count == (OS_DEBUG_SERIAL ? 4008 : 0));
    assert(capture_count == (OS_REALTEST ? 4008 : 0));
}
'''
with tempfile.TemporaryDirectory(prefix='stage-protocol-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(code)
    for serial, capture in ((0, 0), (1, 0), (0, 1), (1, 1)):
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-function',
                        '-pthread', '-fsanitize=address,undefined',
                        f'-DOS_DEBUG_SERIAL={serial}', f'-DOS_REALTEST={capture}',
                        '-I', str(ROOT / 'include'), str(path / 'test.c'),
                        '-o', str(path / 'test')], check=True)
        subprocess.run([str(path / 'test')], check=True)
print('Stage log: neutral consumers, full 64-bit status/sequence, four modes, guarded capture and 4000 concurrent records per mode passed')
