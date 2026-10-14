"""Production filesystem query output must match the two host x86-64 ABIs."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'user/libc/posix_compat.c').read_text()
start = source.index('typedef struct libc_statvfs_abi {')
end = source.index('\nssize_t sendfile(', start)
production = source[start:end]
assert 'liteos_statvfs' not in production
code = '''
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
''' + production.replace('int statvfs(', 'int runtime_statvfs(').replace(
    'int statfs(', 'int runtime_statfs(').replace('int fstatfs(', 'int runtime_fstatfs(') + '''
_Static_assert(sizeof(libc_statfs_abi_t) == sizeof(struct statfs), "statfs size");
_Static_assert(sizeof(libc_statvfs_abi_t) == sizeof(struct statvfs), "statvfs size");
'''
for kind, fields in (
    ('statfs', 'f_type f_bsize f_blocks f_bfree f_bavail f_files f_ffree f_fsid f_namelen f_frsize f_flags f_spare'),
    ('statvfs', 'f_bsize f_frsize f_blocks f_bfree f_bavail f_files f_ffree f_favail f_fsid f_flag f_namemax')):
    for field in fields.split():
        code += f'_Static_assert(offsetof(libc_{kind}_abi_t, {field}) == offsetof(struct {kind}, {field}), "{kind}.{field}");\n'
code += r'''
int main(void) {
    struct { unsigned long before; struct statfs value; unsigned long after; } fs;
    struct { unsigned long before; struct statvfs value; unsigned long after; } vfs;
    memset(&fs, 0xa5, sizeof(fs)); memset(&vfs, 0xa5, sizeof(vfs));
    unsigned long guard = fs.before;
    struct statfs expected = {0};
    expected.f_bsize = 4096; expected.f_frsize = 4096;
    expected.f_blocks = 2097152; expected.f_bfree = 1048576;
    expected.f_bavail = 1048576; expected.f_files = 65536;
    expected.f_ffree = 65535; expected.f_namelen = 255;
    assert(runtime_statfs("/dev/shm", &fs.value) == 0);
    assert(memcmp(&fs.value, &expected, sizeof(expected)) == 0);
    assert(fs.before == guard && fs.after == guard);
    memset(&fs.value, 0xa5, sizeof(fs.value));
    assert(runtime_fstatfs(3, &fs.value) == 0);
    assert(memcmp(&fs.value, &expected, sizeof(expected)) == 0);
    assert(fs.before == guard && fs.after == guard);
    assert(runtime_statvfs("/dev/shm", &vfs.value) == 0);
    assert(vfs.value.f_bsize == 4096 && vfs.value.f_frsize == 4096);
    assert(vfs.value.f_bavail == 1048576 && vfs.value.f_favail == 65535);
    assert(vfs.value.f_namemax == 255 && vfs.before == guard && vfs.after == guard);
    struct statfs saved = fs.value;
    errno = 0; assert(runtime_statfs(NULL, &fs.value) == -1 && errno == EINVAL);
    assert(memcmp(&saved, &fs.value, sizeof(saved)) == 0);
    errno = 0; assert(runtime_statfs("/", NULL) == -1 && errno == EFAULT);
    errno = 0; assert(runtime_statvfs(NULL, &vfs.value) == -1 && errno == EINVAL);
    errno = 0; assert(runtime_statvfs("/", NULL) == -1 && errno == EFAULT);
    errno = 0; assert(runtime_fstatfs(3, NULL) == -1 && errno == EFAULT);
}
'''
with tempfile.TemporaryDirectory(prefix='libc-statfs-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('Filesystem ABI: statfs/statvfs sizes, all field offsets, exact bytes, canary bounds and null-error guards passed')
