"""Execute symlink argument routing and remove libc's former fake success."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/syscall/linux.c').read_text()
begin = source.index('static int64_t linux_symlink(')
helper = source[begin:source.index('static int64_t linux_unlinkat(', begin)]
begin = source.index('    case LINUX_SYS_SYMLINK:')
end = source.index('    case LINUX_SYS_', source.index('case LINUX_SYS_SYMLINKAT:', begin)+26)
dispatch = source[begin:end]
libc = (root/'user/libc/syscall.c').read_text()
begin = libc.index('int symlink(')
wrapper = libc[begin:libc.index('int chmod(', begin)]
program = r'''
#include <assert.h>
#include <string.h>
#include <kernel/base.h>
#include <uapi/linux_abi.h>
#define LINUX_MAX_PATH 256
typedef int process_t;
typedef struct {uint64_t rax,rdi,rsi,rdx;} arch_trap_frame_t;
static int calls, result, path_error, resolve_error;
static const char *expected_target, *expected_path;
static kstatus_t linux_copy_string(uint64_t pointer,char *out,size_t size) {
 if(!pointer)return K_EFAULT;
 const char *s=(const char*)(uintptr_t)pointer;
 if(strlen(s)>=size)return K_ENAMETOOLONG;
 strcpy(out,s);return K_OK;
}
static kstatus_t linux_path_from_user(process_t *p,int64_t fd,uint64_t ptr,char *out) {
 assert(p && fd==LINUX_AT_FDCWD);if(path_error)return path_error;
 return linux_copy_string(ptr,out,256);
}
static kstatus_t vfs_resolve_path_kernel(const char *path,bool follow,bool missing,char *out) {
 assert(!follow && missing);if(resolve_error)return resolve_error;
 strcpy(out,path);return K_OK;
}
static kstatus_t vfs_symlink_kernel(const char *target,const char *path) {
 ++calls;assert(!strcmp(target,expected_target)&&!strcmp(path,expected_path));return result;
}
''' + helper + r'''
static int64_t dispatch(process_t *process,arch_trap_frame_t *frame) {
 switch(frame->rax) {
''' + dispatch + r'''
 default:return K_ENOSYS;
 }
}
static bool personality=true;
static int errno;
#define ENOSYS 38
static bool libc_linux_personality(void){return personality;}
static int __libc_status_result(int64_t status){if(status<0){errno=(int)-status;return -1;}return (int)status;}
static int64_t __libc_linux_syscall6(uint64_t n,uint64_t a,uint64_t b,uint64_t c,
                                    uint64_t d,uint64_t e,uint64_t f) {
 assert(n==88 && !c && !d && !e && !f);
 process_t process=1;arch_trap_frame_t frame={n,a,b,c};return dispatch(&process,&frame);
}
''' + wrapper + r'''
int main(void) {
 process_t process=1;expected_target="localhost-199";expected_path="/tmp/SingletonLock";
 arch_trap_frame_t frame={88,(uintptr_t)expected_target,(uintptr_t)expected_path,0};
 assert(dispatch(&process,&frame)==K_OK && calls==1);
 frame=(arch_trap_frame_t){266,(uintptr_t)expected_target,(uint64_t)LINUX_AT_FDCWD,(uintptr_t)expected_path};
 assert(dispatch(&process,&frame)==K_OK && calls==2);
 path_error=K_EFAULT;assert(dispatch(&process,&frame)==K_EFAULT && calls==2);path_error=0;
 resolve_error=K_ELOOP;assert(dispatch(&process,&frame)==K_ELOOP && calls==2);resolve_error=0;
 assert(linux_symlink(&process,0,LINUX_AT_FDCWD,(uintptr_t)expected_path)==K_EFAULT);
 result=K_EEXIST;assert(symlink(expected_target,expected_path)==-1 && errno==17);
 result=K_ENOMEM;assert(symlink(expected_target,expected_path)==-1 && errno==12);
 result=K_OK;assert(symlink(expected_target,expected_path)==0);
 int before=calls;personality=false;
 assert(symlink(expected_target,expected_path)==-1 && errno==ENOSYS && calls==before);
}
'''
with tempfile.TemporaryDirectory(prefix='linux-symlink-') as directory:
    base = Path(directory)
    (base/'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I'+str(root/'include'), str(base/'test.c'), '-o', str(base/'test')],
                   check=True)
    subprocess.run([str(base/'test')], check=True)
print('Linux/libc symlink: actual dispatch arguments, nofollow creation, '
      'fault/loop/collision/allocation failures and no fake libc success passed')
