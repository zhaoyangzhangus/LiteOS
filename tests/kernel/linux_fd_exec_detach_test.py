"""Actual CLOEXEC compaction, release order and debug descriptor identities."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/syscall/linux.c').read_text()
function = source[source.index('void linux_fd_table_close_on_exec('):
                  source.index('static kstatus_t linux_fd_lookup(')]
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#define OS_LINUX_FD_LIMIT 256
#define LINUX_O_CLOEXEC 0x80000U
#define LINUX_FD_UNUSED 0
typedef struct { uint64_t token; unsigned flags; unsigned char kind; } linux_fd_entry_t;
typedef struct { bool held; } spinlock_t;
typedef struct { spinlock_t lock; linux_fd_entry_t entries[256]; } linux_fd_table_t;
typedef struct { uint64_t pid; linux_fd_table_t linux_fds; } process_t;
static process_t *current;
static linux_fd_entry_t before[256];
static unsigned releases, next, zeroes, log6, log103;
static uint32_t g_debug_linux_exec_fd6_logs;
static void linux_zero(void *p, size_t size) { ++zeroes; memset(p, 0, size); }
static void spinlock_lock(spinlock_t *l) { assert(!l->held); l->held = true; }
static void spinlock_unlock(spinlock_t *l) { assert(l->held); l->held = false; }
static void assert_detached(void) {
    assert(!current->linux_fds.lock.held);
    for (unsigned i=0; i<256; ++i) {
        linux_fd_entry_t *e = &current->linux_fds.entries[i];
        if (before[i].flags & LINUX_O_CLOEXEC) assert(!e->token && !e->flags && !e->kind);
        else assert(!memcmp(e, &before[i], sizeof(*e)));
    }
}
static void linux_fd_entry_put(linux_fd_entry_t *entry) {
    assert_detached();
    while (next < 256 && !(before[next].flags & LINUX_O_CLOEXEC)) ++next;
    assert(next < 256 && !memcmp(entry, &before[next], sizeof(*entry)));
    ++next; ++releases;
}
static void linux_fd_release_locks(process_t *p, const linux_fd_entry_t *entry) {
    assert(p == current && p->linux_fds.lock.held && entry);
}
static void linux_fd_notify_locks(const linux_fd_entry_t *entry) {
    assert(entry); assert_detached();
}
static void kernel_serial_printf_serial_only(const char *format, ...) {
    (void)format; assert_detached(); ++log6;
    va_list args; va_start(args, format);
    assert(va_arg(args, unsigned long long) == current->pid);
    assert(va_arg(args, unsigned) == before[6].kind);
    assert(va_arg(args, unsigned) == before[6].flags);
    assert(va_arg(args, unsigned) == ((before[6].flags & LINUX_O_CLOEXEC) && before[6].kind ? 1U : 0U));
    va_end(args);
}
static void linux_debug_fd103(const char *operation, process_t *process, uint32_t kind) {
    assert_detached(); ++log103;
    assert(!strcmp(operation, "CLOEXEC") && process == current && kind == before[103].kind);
}
'''
checks = r'''
int main(void) {
    (void)linux_zero; (void)kernel_serial_printf_serial_only; (void)linux_debug_fd103;
    for (unsigned pattern=0; pattern<516; ++pattern) {
        process_t process = {.pid=42}; current = &process;
        unsigned expected = 0;
        for (unsigned i=0; i<256; ++i) {
            bool close = pattern < 256 ? i == pattern :
                         pattern < 512 ? i != pattern-256 :
                         pattern == 512 ? false : pattern == 513 ? true : i%2 == pattern%2;
            process.linux_fds.entries[i] = (linux_fd_entry_t){.token=i+1,
                .flags=17U | (close ? LINUX_O_CLOEXEC : 0U), .kind=(i+pattern)%3 ? 1 : 0};
            if (close) ++expected;
        }
        memcpy(before, process.linux_fds.entries, sizeof(before));
        releases = next = zeroes = log6 = log103 = 0;
        g_debug_linux_exec_fd6_logs = pattern%2 ? 128 : 0;
        linux_fd_table_close_on_exec(NULL);
        linux_fd_table_close_on_exec(&process);
        assert_detached(); assert(releases == expected && !zeroes);
#if OS_DEBUG_SERIAL
        assert(log6 == (before[6].kind && !(pattern%2) ? 1U : 0U));
        assert(log103 == (before[103].kind && (before[103].flags & LINUX_O_CLOEXEC) ? 1U : 0U));
#else
        assert(!log6 && !log103);
#endif
        memcpy(before, process.linux_fds.entries, sizeof(before));
        linux_fd_table_close_on_exec(&process);
        assert(releases == expected && !zeroes); assert_detached();
    }
    puts("516 CLOEXEC layouts: exact compact release count/order, untouched survivors and debug identities passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-fd-exec-') as directory:
    path = Path(directory)
    def run(body, debug):
        (path/'test.c').write_text(fixture + body + checks)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        '-no-pie', f'-DOS_DEBUG_SERIAL={debug}', str(path/'test.c'),
                        '-o', str(path/'test')], check=True)
        return subprocess.run([str(path/'test')], capture_output=True, text=True)
    for debug in (0, 1):
        result = run(function, debug)
        assert result.returncode == 0, result.stdout + result.stderr
        print(f'debug={debug}: {result.stdout.strip()}')
    negative = function.replace('entries[entry_count++] =', 'entries[entry_count] =')
    assert run(negative, 1).returncode != 0, 'Missing detached entry count passed'
    negative = function.replace('descriptor_6_removed ? 1U : 0U',
                                'entries[6].kind != LINUX_FD_UNUSED ? 1U : 0U')
    negative = negative.replace('kernel_serial_printf_serial_only(',
                                '(void)descriptor_6_removed; kernel_serial_printf_serial_only(')
    assert run(negative, 1).returncode != 0, 'Compacted index mistaken for descriptor 6 passed'
    print('Negative controls rejected: lost releases and wrong debug descriptor identity')
