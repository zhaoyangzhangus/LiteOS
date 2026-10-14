"""Exercise real readlink translation needed by glibc realpath."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/syscall/linux.c').read_text()
begin = source.index('static int64_t linux_readlinkat(')
end = source.index('\n#define LINUX_PROC_DATA_MAX', begin)
program = r'''
#include <assert.h>
#include <string.h>
#include <kernel/base.h>
#include <uapi/linux_abi.h>
#define LINUX_MAX_PATH 256
#define LINUX_EFAULT K_EFAULT
#define __user
typedef struct { char linux_exe[256]; } process_t;
typedef struct { unsigned type; } os_file_info_t;
static int copied, stat_calls, copy_failure;
static kstatus_t linux_path_from_user(process_t *p, int64_t fd, uint64_t pointer,
                                     char *out) {
    assert(p && fd == LINUX_AT_FDCWD);
    if (!pointer) return K_EFAULT;
    const char *path = (const char *)(uintptr_t)pointer;
    if (!*path) return K_ENOENT;
    strcpy(out, path);
    return K_OK;
}
static bool linux_exact_path(const char *path, const char *expected) {
    return !strcmp(path, expected);
}
static kstatus_t vfs_stat_kernel(const char *path, os_file_info_t *info) {
    ++stat_calls;
    info->type = 0;
    if (!strcmp(path, "/tmp/file") || !strcmp(path, "/tmp/profile")) return K_OK;
    if (!strcmp(path, "/error")) return K_EIO;
    return K_ENOENT;
}
static kstatus_t vfs_resolve_path_kernel(const char *path, bool follow, bool missing,
                                        char *out) {
    assert(!follow && !missing); strcpy(out, path); return K_OK;
}
static kstatus_t vfs_readlink_kernel(const char *path, char *out, size_t size, size_t *bytes) {
    if (!strcmp(path, "/tmp/link")) {
        assert(size >= 6); memcpy(out, "target", 6); *bytes = 6; return K_OK;
    }
    os_file_info_t info;
    kstatus_t status = vfs_stat_kernel(path, &info);
    return status == K_OK ? K_EINVAL : status;
}
static bool linux_user_pointer(uint64_t p, size_t size) {
    return p > 4096 && size <= 256;
}
static kstatus_t copy_to_user(void *out, const void *in, size_t count) {
    ++copied;
    if (copy_failure) return K_EFAULT;
    memcpy(out, in, count);
    return K_OK;
}
''' + source[begin:end] + r'''
int main(void) {
    process_t process = {"/sbin/chrome"};
    char out[32]; memset(out, 'X', sizeof(out));
#define READ(path, size) linux_readlinkat(&process, LINUX_AT_FDCWD, \
                             (uintptr_t)(path), (uintptr_t)out, (size))
    assert(READ("/tmp/profile", 32) == K_EINVAL);
    assert(READ("/tmp/file", 32) == K_EINVAL);
    assert(READ("/absent", 32) == K_ENOENT);
    assert(READ("/error", 32) == K_EIO && !copied && stat_calls == 4);
    assert(READ("/tmp/link", 3) == 3 && !memcmp(out, "tar", 3) && out[3] == 'X');
    assert(READ(0, 32) == K_EFAULT);
    assert(READ("", 32) == K_ENOENT);
    assert(READ("/proc/self/exe", 0) == K_EINVAL && copied == 1);
    assert(READ("/proc/self/exe", 8) == 8 && !memcmp(out, "/sbin/ch", 8));
    assert(out[8] == 'X'); // readlink must not append a terminator.
    assert(READ("/proc/self/exe", 32) == 12 && !memcmp(out, "/sbin/chrome", 12));
    assert(out[12] == 'X');
    assert(linux_readlinkat(&process, LINUX_AT_FDCWD, (uintptr_t)"/proc/self/exe",
                           0, 32) == K_EFAULT);
    copy_failure = 1;
    assert(READ("/proc/self/exe", 32) == K_EFAULT);
    process.linux_exe[0] = 0;
    assert(READ("/proc/self/exe", 32) == K_ENOENT);
}
'''
with tempfile.TemporaryDirectory(prefix='linux-readlink-') as directory:
    base = Path(directory)
    (base/'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I'+str(root/'include'), str(base/'test.c'), '-o', str(base/'test')],
                   check=True)
    subprocess.run([str(base/'test')], check=True)
print('Linux readlink: existing non-links versus absent paths, VFS errors, '
      'proc executable truncation/no terminator, empty buffers and user-copy faults passed')
