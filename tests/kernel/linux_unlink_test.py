"""Execute actual Linux deletion dispatch, nofollow routing and errors."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root/'kernel/syscall/linux.c').read_text()
begin = source.index('static int64_t linux_unlinkat(')
helper = source[begin:source.index('static int64_t linux_renameat(', begin)]
begin = source.index('    case LINUX_SYS_UNLINK:')
dispatch = source[begin:source.index('    case LINUX_SYS_MKDIR:', begin)]
code = r'''
#include <assert.h>
#include <string.h>
#include <kernel/base.h>
#include <uapi/linux_abi.h>
#define LINUX_MAX_PATH 256
typedef int process_t;
typedef struct {uint64_t rax,rdi,rsi,rdx;} frame_t;
static int path_error,resolve_error,result,calls,wanted_directory;
static int64_t wanted_fd=LINUX_AT_FDCWD;
static kstatus_t linux_path_from_user(process_t *p,int64_t fd,uint64_t ptr,char *out) {
 assert(p && fd==wanted_fd);
 if(path_error)return path_error;
 if(!ptr)return K_EFAULT;
 strcpy(out,(const char*)(uintptr_t)ptr);return K_OK;
}
static kstatus_t vfs_resolve_path_kernel(const char *path,bool follow,bool missing,char *out) {
 assert(!follow && !missing);
 if(resolve_error)return resolve_error;
 strcpy(out,path);return K_OK;
}
static kstatus_t vfs_remove_typed_kernel(const char *path,int directory) {
 ++calls;assert(!strcmp(path,"/tmp/SingletonLock") && directory==wanted_directory);
 return result;
}
''' + helper + '\nstatic int64_t dispatch(process_t *process,frame_t *frame) {switch(frame->rax) {\n' + dispatch + r'''
 default:return K_ENOSYS;
}}
int main(void) {
 process_t p=1;const char *path="/tmp/SingletonLock";
 frame_t f={87,(uintptr_t)path,0,0};
 assert(dispatch(&p,&f)==K_OK && calls==1);
 wanted_directory=1;f.rax=84;assert(dispatch(&p,&f)==K_OK && calls==2);
 wanted_fd=42;wanted_directory=0;f=(frame_t){263,42,(uintptr_t)path,0};
 assert(dispatch(&p,&f)==K_OK && calls==3);
 wanted_directory=1;f.rdx=LINUX_AT_REMOVEDIR;
 result=K_ENOTEMPTY;assert(dispatch(&p,&f)==K_ENOTEMPTY && calls==4);
 f.rdx=1;assert(dispatch(&p,&f)==K_EINVAL && calls==4);
 f.rdx=0;f.rsi=0;assert(dispatch(&p,&f)==K_EFAULT && calls==4);
 f.rsi=(uintptr_t)path;path_error=K_ENOTDIR;
 assert(dispatch(&p,&f)==K_ENOTDIR && calls==4);path_error=0;
 resolve_error=K_ELOOP;assert(dispatch(&p,&f)==K_ELOOP && calls==4);resolve_error=0;
 f.rsi=(uintptr_t)"/";assert(dispatch(&p,&f)==K_EBUSY && calls==4);
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)/'unlink.c'
    path.write_text(code)
    binary = Path(directory)/'unlink'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I'+str(root/'include'), str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('Linux unlink/rmdir/unlinkat: real dispatch, nofollow leaf, directory fd, flags, failures and root rejection passed')
