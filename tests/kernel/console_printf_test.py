"""Exercise the actual kernel formatter, including its two independent sinks."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = ROOT / 'kernel/console_printf.c'
assert 'liteos' not in source.read_text().lower()
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define printf test_console_printf
''' + f'#include "{source}"\n' + r'''
#undef printf
static char normal[8192], serial[8192];
static unsigned normal_calls, serial_calls;
void kernel_serial_write(const char *text) {
    assert(strlen(text) < sizeof(normal)); strcpy(normal, text); ++normal_calls;
}
void kernel_serial_write_serial_only(const char *text) {
    assert(strlen(text) < sizeof(serial)); strcpy(serial, text); ++serial_calls;
}
static size_t format(char *out, size_t cap, const char *fmt, ...) {
    va_list args; va_start(args, fmt);
    size_t result = console_vformat(out, cap, fmt, args);
    va_end(args); return result;
}
int main(void) {
    char expected[512], actual[512];
    const char *fmt = "%d %i %u %x %X %ld %lu %lld %llu %zd %zu %p %c %%";
    int n = snprintf(expected, sizeof(expected), fmt, INT32_MIN, -1, UINT32_MAX,
                     UINT32_MAX, UINT32_MAX, (long)INT64_MIN, (unsigned long)UINT64_MAX,
                     (long long)INT64_MIN, (unsigned long long)UINT64_MAX,
                     (ptrdiff_t)INT64_MIN, (size_t)UINT64_MAX, (void *)(uintptr_t)0x1234, 'Q');
    assert(format(actual, sizeof(actual), fmt, INT32_MIN, -1, UINT32_MAX,
                  UINT32_MAX, UINT32_MAX, (long)INT64_MIN, (unsigned long)UINT64_MAX,
                  (long long)INT64_MIN, (unsigned long long)UINT64_MAX,
                  (ptrdiff_t)INT64_MIN, (size_t)UINT64_MAX, (void *)(uintptr_t)0x1234, 'Q') == (size_t)n);
    assert(!strcmp(actual, expected));
    for (size_t cap = 0; cap <= 32; ++cap) {
        unsigned char guarded[40]; memset(guarded, 0xa5, sizeof(guarded));
        assert(format((char *)guarded + 1, cap, "%llu", (unsigned long long)UINT64_MAX) == 20);
        assert(guarded[0] == 0xa5);
        for (size_t i = cap + 1; i < sizeof(guarded); ++i) assert(guarded[i] == 0xa5);
        if (cap) {
            size_t len = cap <= 20 ? cap - 1 : 20;
            assert(!memcmp(guarded + 1, "18446744073709551615", len));
            assert(guarded[len + 1] == 0);
        }
    }
    assert(format(NULL, 0, "%s", "hello") == 5);
    assert(format(actual, sizeof(actual), "%s %s", NULL, "") == 7);
    assert(!strcmp(actual, "(null) "));
    assert(format(actual, sizeof(actual), "abc%q%") == 6);
    assert(!strcmp(actual, "abc%q%"));
    assert(printk("%d:%s", 42, "ok") == 5 && !strcmp(normal, "42:ok"));
    assert(normal_calls == 1 && serial_calls == 0);
    assert(kernel_serial_printf_serial_only("%d:%s", 7, "serial") == 8);
    assert(!strcmp(serial, "7:serial") && normal_calls == 1 && serial_calls == 1);
    assert(test_console_printf("%s", NULL) == 6 && !strcmp(normal, "(null)"));
    char long_text[4097]; memset(long_text, 'a', sizeof(long_text) - 1); long_text[4096] = 0;
    assert(printk("%s", long_text) == 4096 && strlen(normal) == 4096);
    assert(printk("prefix:%s", long_text) == 4103 && strlen(normal) == 2047);
    assert(!strncmp(normal, "prefix:", 7));
    assert(printk(NULL) == 0 && !strcmp(normal, ""));
    puts("CONSOLE_PRINTF_TEST_OK");
}
'''
with tempfile.TemporaryDirectory(prefix='console-printf-') as tmp:
    fixture = Path(tmp) / 'test.c'
    binary = Path(tmp) / 'test'
    fixture.write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fno-builtin',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I', str(ROOT / 'include'), str(fixture), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
