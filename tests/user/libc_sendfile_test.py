"""Exercise the real sendfile loop with deterministic I/O faults and short I/O."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'user/libc/posix_compat.c').read_text()
start = source.index('ssize_t sendfile(')
end = source.index('\n}', start) + 2
code = r'''
#define _POSIX_C_SOURCE 200809L
#include <sys/types.h>
#include <stdint.h>
#include <limits.h>
#include <errno.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
static unsigned char input[40001], output[40001];
static size_t input_size, output_size, write_limit, stop_after, read_limit;
static off_t cursor;
static int read_interrupt, write_interrupt, read_failure, write_zero, seek_failure;
static ssize_t mock_pread(int fd, void *data, size_t length, off_t pos) {
    assert(fd == 3 && pos >= 0 && length <= 16384);
    if (read_interrupt) {read_interrupt = 0; errno = EINTR; return -1;}
    if (read_failure) {errno = EIO; return -1;}
    if ((uint64_t)pos >= input_size) return 0;
    if (length > input_size - (size_t)pos) length = input_size - (size_t)pos;
    if (length > read_limit) length = read_limit;
    memcpy(data, input + pos, length);
    return (ssize_t)length;
}
static ssize_t mock_write(int fd, const void *data, size_t length) {
    assert(fd == 4);
    if (write_interrupt) {write_interrupt = 0; errno = EINTR; return -1;}
    if (write_zero) return 0;
    if (output_size == stop_after) {errno = EAGAIN; return -1;}
    if (length > write_limit) length = write_limit;
    if (length > stop_after - output_size) length = stop_after - output_size;
    assert(output_size + length <= sizeof(output));
    memcpy(output + output_size, data, length); output_size += length;
    return (ssize_t)length;
}
static off_t mock_lseek(int fd, off_t pos, int whence) {
    assert(fd == 3);
    if (seek_failure) {errno = ESPIPE; return -1;}
    if (whence == SEEK_CUR) {assert(pos == 0); return cursor;}
    assert(whence == SEEK_SET); cursor = pos; return pos;
}
#define pread mock_pread
#define write mock_write
#define lseek mock_lseek
''' + source[start:end].replace('ssize_t sendfile(', 'ssize_t runtime_sendfile(') + r'''
static void reset(void) {
    input_size = sizeof(input); output_size = 0; write_limit = sizeof(input);
    stop_after = sizeof(input); read_limit = sizeof(input); cursor = 0;
    read_interrupt = write_interrupt = read_failure = write_zero = seek_failure = 0;
    memset(output, 0xcc, sizeof(output));
}
int main(void) {
    for (size_t i = 0; i < sizeof(input); ++i) input[i] = (unsigned char)(i * 31);
    for (size_t limit = 1; limit <= 17000; limit = limit * 3 + 1) {
        for (int explicit_offset = 0; explicit_offset < 2; ++explicit_offset) {
            reset(); write_limit = limit; read_limit = 1013;
            read_interrupt = write_interrupt = 1;
            off_t pos = 7; cursor = 7;
            assert(runtime_sendfile(4, 3, explicit_offset ? &pos : NULL, SIZE_MAX) == 39994);
            assert(output_size == 39994 && !memcmp(input + 7, output, output_size));
            assert(explicit_offset ? (pos == 40001 && cursor == 7) : cursor == 40001);
            assert(runtime_sendfile(4, 3, explicit_offset ? &pos : NULL, 1) == 0);
        }
    }
    for (int explicit_offset = 0; explicit_offset < 2; ++explicit_offset) {
        reset(); stop_after = 17; off_t pos = 0;
        assert(runtime_sendfile(4, 3, explicit_offset ? &pos : NULL, 100) == 17);
        assert(explicit_offset ? (pos == 17 && cursor == 0) : cursor == 17);
        stop_after = sizeof(input);
        assert(runtime_sendfile(4, 3, explicit_offset ? &pos : NULL, 83) == 83);
        assert(output_size == 100 && !memcmp(input, output, 100));
    }
    reset(); input_size = 0; assert(runtime_sendfile(4, 3, NULL, 100) == 0);
    reset(); stop_after = 0; assert(runtime_sendfile(4, 3, NULL, 100) == -1 && errno == EAGAIN && cursor == 0);
    reset(); write_zero = 1; assert(runtime_sendfile(4, 3, NULL, 100) == -1 && errno == EIO && cursor == 0);
    reset(); read_failure = 1; assert(runtime_sendfile(4, 3, NULL, 100) == -1 && errno == EIO);
    reset(); seek_failure = 1; assert(runtime_sendfile(4, 3, NULL, 100) == -1 && errno == ESPIPE);
    off_t pos = -1; assert(runtime_sendfile(4, 3, &pos, 100) == -1 && errno == EINVAL);
    pos = INT64_MAX; assert(runtime_sendfile(4, 3, &pos, 100) == -1 && errno == EOVERFLOW);
    assert(runtime_sendfile(4, 3, &pos, 0) == 0 && pos == INT64_MAX);
}
'''
with tempfile.TemporaryDirectory(prefix='libc-sendfile-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('Sendfile: real loop, chunk boundaries, short writes, EINTR, EOF, EAGAIN resume without loss, offsets and error guards passed')
