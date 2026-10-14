"""Exercise actual window routing and rejection of the retired device path."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
kernel = (ROOT / 'kernel/syscall/linux.c').read_text()
libc = (ROOT / 'user/libc/syscall.c').read_text()

def block(source, start):
    opening = source.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

exact = block(kernel, kernel.index('static bool linux_exact_path('))
route = block(kernel, kernel.index('    if (linux_exact_path(path, LINUX_WINDOW_DEVICE_PATH)'))
classifier = block(libc, libc.index('static bool linux_abi_path('))
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <uapi/linux_abi.h>
_Static_assert(LINUX_SYS_WINDOW_CREATE == 450U, "window syscall unchanged");
_Static_assert(LINUX_SYS_DISPLAY_GET_INFO == 451U, "display syscall unchanged");
_Static_assert(sizeof(linux_window_create_t) == 80, "create ABI unchanged");
_Static_assert(offsetof(linux_window_create_t, address) == 24, "address ABI unchanged");
_Static_assert(sizeof(linux_window_command_t) == 48, "command ABI unchanged");
enum {K_OK=0, K_ENOENT=-2, K_EINVAL=-22, K_EACCES=-13, K_EMFILE=-24, LINUX_FD_WINDOW=7};
static int owner, object, lookups, installs, puts;
static int lookup_status;
static int64_t install_status;
static uint32_t installed_flags;
static int window_server_lookup_owner(void *process, void **window) {
    assert(process == &owner); ++lookups;
    if (!lookup_status) *window=&object;
    return lookup_status;
}
static int64_t linux_fd_install(void *process, int fd, int zero, void *window,
                                int kind, uint32_t flags) {
    assert(process==&owner && fd==-1 && !zero && window==&object && kind==LINUX_FD_WINDOW);
    ++installs; installed_flags=flags; return install_status;
}
static void window_server_put(void *window) {assert(window==&object); ++puts;}
''' + exact + '\n' + classifier + r'''
static int64_t open_window(const char *path, uint64_t flags) {
    void *process=&owner, *window=NULL;
    int status;
''' + route + r'''
    return K_ENOENT;
}
static void reset(void) {
    lookups=installs=puts=0; lookup_status=0; install_status=17; installed_flags=0;
}
int main(void) {
    assert(!strcmp(LINUX_WINDOW_DEVICE_PATH,"/dev/system-window"));
    const char *paths[]={LINUX_WINDOW_DEVICE_PATH};
    for (unsigned i=0;i<sizeof(paths)/sizeof(*paths);++i) {
        assert(linux_abi_path(paths[i]));
        reset(); assert(open_window(paths[i],LINUX_O_RDWR|LINUX_O_CLOEXEC)==17);
        assert(lookups==1 && installs==1 && !puts);
        assert(installed_flags==(LINUX_O_RDWR|LINUX_O_CLOEXEC));
        const uint32_t forbidden[]={LINUX_O_CREAT,LINUX_O_EXCL,LINUX_O_TRUNC,LINUX_O_DIRECTORY};
        for (unsigned j=0;j<4;++j) {
            reset(); assert(open_window(paths[i],forbidden[j])==K_EINVAL);
            assert(!lookups && !installs && !puts);
        }
        reset(); lookup_status=K_EACCES;
        assert(open_window(paths[i],0)==K_EACCES && lookups==1 && !installs && !puts);
        reset(); install_status=K_EMFILE;
        assert(open_window(paths[i],0)==K_EMFILE && lookups==1 && installs==1 && puts==1);
    }
    const char *invalid[]={NULL,"","/dev/system-window-extra","/dev/system-window/",
                          "/dev/liteos-window","/dev/liteos-window-extra","/dev/liteos-window/","/dev/System-window"};
    for (unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);++i) {
        reset(); assert(!linux_abi_path(invalid[i]));
        assert(open_window(invalid[i],0)==K_ENOENT && !lookups && !installs && !puts);
    }
    assert(linux_abi_path("/proc/self/exe") && linux_abi_path("/tmp/profile") && linux_abi_path("/dev/nvidia0"));
}
'''
with tempfile.TemporaryDirectory(prefix='window-device-namespace-') as directory:
    binary = Path(directory) / 'test'
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I', str(ROOT/'include'), '-x', 'c', '-', '-o', str(binary)],
                   input=code, text=True, check=True)
    subprocess.run([str(binary)], check=True)
for name in ('LINUX_SYS_WINDOW_CREATE','LINUX_SYS_DISPLAY_GET_INFO'):
    assert f'case {name}:' in kernel and f'case {name}:' in libc, name
for name in ('opengl_cube_demo.c', 'phase11_triangle_test.c'):
    for headers in ([], ['-I', str(ROOT/'user/libc/include')]):
        subprocess.run(['cc', '-fsyntax-only', '-Werror=incompatible-pointer-types',
                        '-I', str(ROOT/'include'), *headers,
                        str(ROOT/'tools/nvidia'/name)], check=True)
print('Window namespace: stable syscall/layout ABI, retired path rejection, exact routing, owner/FD failure cleanup passed')
