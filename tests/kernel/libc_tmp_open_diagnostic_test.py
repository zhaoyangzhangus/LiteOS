"""Verify failure-only diagnostic bounds and errno preservation."""
import pathlib
import subprocess
import tempfile

source = pathlib.Path('user/libc/syscall.c').read_text()
budget = source[source.index('static bool libc_trace_budget('):source.index('static void libc_debug_text(')]
helper = budget + source[source.index('static void libc_tmp_open_failure('):source.index('static void close_raw_linux_descriptor(')]
program = '''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
static int emitted;
static int mock_write(int fd, const void *buffer, size_t bytes) {
 assert(fd == 2); assert(bytes < 384);
 assert(strstr(buffer, "stage=openat status=-28 errno=28") != NULL);
 ++emitted; errno = EIO; return -1;
}
#define write mock_write
''' + helper + '''
int main(void) {
 errno = ENOSPC;
 for (int i = 0; i < 100; ++i) {
  libc_tmp_open_failure("openat", "/tmp/no-file", -ENOENT);
  libc_tmp_open_failure("openat", "/tmp/existing", -EEXIST);
  libc_tmp_open_failure("openat", "/outside", -ENOSPC);
 }
 assert(emitted == 0);
 for (int i = 0; i < 100; ++i) {
  libc_tmp_open_failure("openat", "/tmp/shared", -ENOSPC);
  assert(errno == ENOSPC);
 }
 assert(emitted == 16);
}
'''
with tempfile.TemporaryDirectory() as directory:
    c = pathlib.Path(directory) / 'test.c'
    c.write_text(program)
    exe = pathlib.Path(directory) / 'test'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('Temporary open diagnostic: noise filtering, bounded output and errno passed')
