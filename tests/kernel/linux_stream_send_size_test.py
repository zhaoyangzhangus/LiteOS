"""Exercise the production send/write adapter with large TCP writes."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/syscall/linux.c').read_text()
start = source.index('static int64_t linux_socket_send(socket_t *socket,')
end = source.index('static int64_t linux_message_load(', start)
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#define SOCKET_MAX_PAYLOAD 4096U
#define OS_SOCK_STREAM 1
#define OS_SOCK_DGRAM 2
#define K_EINVAL -22
#define LINUX_EMSGSIZE -90
#define LINUX_EFAULT -14
#define K_OK 0
#define __user
typedef struct { unsigned type; } socket_t;
typedef struct { unsigned flags; } linux_fd_entry_t;
static size_t copied, sent, accepted;
static int send_status;
static bool linux_user_pointer(uint64_t p, size_t n) { return p && n; }
static int copy_from_user(void *dst, const void *src, size_t n) {
    copied = n; memcpy(dst, src, n); return K_OK;
}
static int64_t linux_socket_send_payload(socket_t *s,
    const linux_fd_entry_t *e, const uint8_t *p, size_t n,
    uint32_t flags, uint64_t address, uint64_t address_length) {
    (void)s; (void)e; (void)flags; (void)address; (void)address_length;
    assert(n <= SOCKET_MAX_PAYLOAD);
    for (size_t i = 0; i < n; ++i) assert(p[i] == (uint8_t)i);
    sent = n;
    return send_status ? send_status : (int64_t)(accepted < n ? accepted : n);
}
''' + source[start:end] + r'''
int main(void) {
    uint8_t data[20000];
    for (size_t i = 0; i < sizeof(data); ++i) data[i] = (uint8_t)i;
    socket_t tcp = {OS_SOCK_STREAM}, udp = {OS_SOCK_DGRAM};
    linux_fd_entry_t entry = {0};
    accepted = SOCKET_MAX_PAYLOAD;
    assert(linux_socket_send(&tcp, &entry, (uintptr_t)data, sizeof(data),
                            0, 0, 0) == SOCKET_MAX_PAYLOAD);
    assert(copied == SOCKET_MAX_PAYLOAD && sent == SOCKET_MAX_PAYLOAD);
    assert(linux_socket_send(&tcp, &entry, (uintptr_t)data, UINT64_MAX,
                            0, 0, 0) == SOCKET_MAX_PAYLOAD);
    size_t offset = 0;
    unsigned writes = 0;
    while (offset < sizeof(data)) {
        int64_t n = linux_socket_send(&tcp, &entry, (uintptr_t)(data + offset),
                                     sizeof(data) - offset, 0, 0, 0);
        assert(n > 0 && (size_t)n <= sizeof(data) - offset);
        offset += (size_t)n;
        ++writes;
    }
    assert(offset == sizeof(data) && writes == 5);
    copied = sent = 0;
    assert(linux_socket_send(&udp, &entry, (uintptr_t)data, sizeof(data),
                            0, 0, 0) == LINUX_EMSGSIZE);
    assert(!copied && !sent);
    assert(linux_socket_send(&udp, &entry, (uintptr_t)data, 100,
                            0, 0, 0) == 100);
    accepted = 512;
    assert(linux_socket_send(&tcp, &entry, (uintptr_t)data, sizeof(data),
                            0, 0, 0) == 512);
    send_status = -11;
    assert(linux_socket_send(&tcp, &entry, (uintptr_t)data, sizeof(data),
                            0, 0, 0) == -11);
    send_status = 0;
    assert(linux_socket_send(&tcp, &entry, 0, 1, 0, 0, 0) == LINUX_EFAULT);
    assert(linux_socket_send(&tcp, &entry, 0, 0, 0, 0, 0) == 0);
    assert(linux_socket_send(0, &entry, 0, 0, 0, 0, 0) == K_EINVAL);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-stream-send-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('TCP large/partial writes, UDP atomicity, EAGAIN and user pointers passed')
