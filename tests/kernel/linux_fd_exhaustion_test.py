"""Run production F_DUPFD allocation with a full table and reference tracking."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/syscall/linux.c').read_text()
start = source.index('static int64_t linux_dup_min(')
end = source.index('static int64_t linux_fstat(', start)
install = source[source.index('static int64_t linux_fd_install_description('):
                 source.index('static kstatus_t linux_fd_detach(')]
assert 'int64_t result = K_EMFILE;' in install
assert 'if (result == K_EMFILE &&' in install
program = r'''
#include <assert.h>
#include <stdint.h>
#define K_EMFILE -24
#define K_EBADF -9
#define K_EINVAL -22
#define K_OK 0
#define OS_LINUX_FD_LIMIT 4
#define LINUX_FD_UNUSED 0
#define LINUX_O_CLOEXEC 0x80000
#define LINUX_FD_CLOEXEC 1
typedef int kstatus_t;
typedef struct { unsigned kind, flags; } linux_fd_entry_t;
typedef struct { struct { int lock; linux_fd_entry_t entries[4]; } linux_fds; } process_t;
static int references;
static void spinlock_lock(int *lock) { (void)lock; }
static void spinlock_unlock(int *lock) { (void)lock; }
static kstatus_t linux_fd_lookup(process_t *p, int64_t fd, linux_fd_entry_t *entry) {
    if (fd < 0 || fd >= 4 || !p->linux_fds.entries[fd].kind) return K_EBADF;
    *entry = p->linux_fds.entries[fd]; ++references; return K_OK;
}
static void linux_fd_entry_put(linux_fd_entry_t *entry) {
    if (entry->kind) { --references; entry->kind = 0; }
}
''' + source[start:end] + r'''
int main(void) {
    process_t p = {0};
    for (int i = 0; i < 4; ++i) p.linux_fds.entries[i].kind = 1;
    assert(linux_dup_min(&p, 0, 0, 0) == K_EMFILE && references == 0);
    assert(linux_dup_min(&p, -1, 0, 0) == K_EBADF && references == 0);
    assert(linux_dup_min(&p, 0, 4, 0) == K_EINVAL && references == 0);
    p.linux_fds.entries[2].kind = 0;
    assert(linux_dup_min(&p, 0, 3, 0) == K_EMFILE && references == 0);
    assert(linux_dup_min(&p, 0, 0, LINUX_FD_CLOEXEC) == 2);
    assert(references == 1 && p.linux_fds.entries[2].flags == LINUX_O_CLOEXEC);
    linux_fd_entry_put(&p.linux_fds.entries[2]);
    p.linux_fds.entries[0].flags = LINUX_O_CLOEXEC;
    assert(linux_dup_min(&p, 0, 2, 0) == 2);
    assert(references == 1 && p.linux_fds.entries[2].flags == 0);
    linux_fd_entry_put(&p.linux_fds.entries[2]);
    assert(references == 0);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-fd-limit-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('FD exhaustion: EMFILE, reference cleanup, slot reuse and CLOEXEC passed')
