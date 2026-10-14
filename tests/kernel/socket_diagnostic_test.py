"""Check actual socket trace call sites and Linux I/O trace rate limiting."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
native = (ROOT / 'kernel/syscall/network.c').read_text()
for name in ('socket.c', 'socket_protocol.c', 'socket_transport.c'):
    assert 'LITEOS_' not in (ROOT / 'kernel/net' / name).read_text(), name
assert 'LITEOS_' not in native
markers = {
    'OS_SOCKET_CREATE', 'OS_SOCKET_CONNECT', 'OS_SOCKET_SEND',
    'OS_SOCKET_RECV_LOOKUP_FAIL', 'OS_SOCKET_RECV_ENTER', 'OS_SOCKET_RECV',
    'OS_SOCKET_RECV_DETAIL', 'OS_SOCKET_RECV6_LOOKUP_FAIL',
    'OS_SOCKET_RECV6_ENTER', 'OS_SOCKET_RECV6',
}
calls = []
seen = set()
for match in re.finditer(r'kernel_serial_printf_serial_only\((.*?)\);', native, re.S):
    first = re.search(r'"(OS_[A-Z0-9_]+) ', match[1])
    if first and first[1] in markers:
        seen.add(first[1]); calls.append(match[1])
assert seen == markers and len(calls) == 11
linux = (ROOT / 'kernel/syscall/linux.c').read_text()
start = linux.index('static void linux_debug_socket_io(')
opening = linux.index('{', start)
depth, end = 1, opening + 1
while depth:
    depth += (linux[end] == '{') - (linux[end] == '}')
    end += 1
helper = linux[start:end]
assert r'\\r\\n' not in helper and 'LITEOS_' not in helper
code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#define printf test_console_printf
''' + f'#include "{ROOT / "kernel/console_printf.c"}"\n' + r'''
#undef printf
int kernel_serial_printf_serial_only(const char *, ...) __attribute__((format(printf, 1, 2)));
static char output[512], expected[512];
static unsigned records;
void kernel_serial_write(const char *text) { (void)text; assert(0); }
void kernel_serial_write_serial_only(const char *text) {
    assert(strlen(text) < sizeof(output)); strcpy(output, text); ++records;
}
static void reference_printf(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void reference_printf(const char *format, ...) {
    va_list ap; va_start(ap,format); vsnprintf(expected,sizeof(expected),format,ap); va_end(ap);
}
static uint64_t family, type, handle, address, port, length, timeout_ns, bytes;
static kstatus_t status;
static uint32_t g_debug_linux_socket_io_logs;
''' + helper
for i, arguments in enumerate(calls):
    code += f'''
static void trace_{i}(void) {{
    reference_printf({arguments});
    kernel_serial_printf_serial_only({arguments});
    assert(!strcmp(output, expected));
    size_t n = strlen(output);
    assert(n >= 2 && output[n-2] == '\\r' && output[n-1] == '\\n');
}}
'''
code += 'static void traces(void) {\n' + ''.join(f'trace_{i}();\n' for i in range(len(calls))) + '}\n'
code += r'''
int main(void) {
    family=type=handle=address=port=length=timeout_ns=bytes=UINT64_MAX;
    status=K_OK; traces();
    assert(strstr(output,"timeout=18446744073709551615"));
    assert(strstr(output,"bytes=18446744073709551615\r\n"));
    status=INT64_MIN; traces();
    assert(strstr(output,"status=-9223372036854775808 bytes=0\r\n"));
    family=type=handle=address=port=length=timeout_ns=bytes=0; status=K_EIO; traces();
    assert(strstr(output,"timeout=0 status=-5 bytes=0\r\n"));
    linux_debug_socket_io(UINT32_MAX, INT64_MIN, INT64_MAX);
    assert(!strcmp(output,"OS_LINUX_SOCKET_IO op=4294967295 fd=-9223372036854775808 result=9223372036854775807\r\n"));
    linux_debug_socket_io(2, 42, K_EAGAIN);
    assert(!strcmp(output,"OS_LINUX_SOCKET_IO op=2 fd=42 result=-11\r\n"));
    unsigned baseline=records;
    for (unsigned i=2; i<256; ++i) linux_debug_socket_io(1, 0, 0);
    assert(records == baseline + 254 && g_debug_linux_socket_io_logs == 256);
    baseline=records;
    for (unsigned i=0; i<1000; ++i) linux_debug_socket_io(1, 0, 0);
    assert(records == baseline && g_debug_linux_socket_io_logs == 256);
    puts("SOCKET_DIAGNOSTIC_TEST_OK");
}
'''
with tempfile.TemporaryDirectory(prefix='socket-diagnostics-') as temporary:
    fixture = Path(temporary) / 'test.c'; binary = Path(temporary) / 'test'
    fixture.write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fno-builtin',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I', str(ROOT / 'include'), str(fixture), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
