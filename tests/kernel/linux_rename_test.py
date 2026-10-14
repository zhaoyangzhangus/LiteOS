"""Execute Linux rename dispatch and parent-only symlink resolution."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/syscall/linux.c').read_text()
start = source.index('static int64_t linux_renameat(')
helper = source[start:source.index('static int64_t linux_sysinfo(', start)]
start = source.index('    case LINUX_SYS_RENAME:')
dispatch = source[start:source.index('    case LINUX_SYS_UNLINK:', start)]
fixture = r'''
#include <assert.h>
#include <string.h>
#include <kernel/base.h>
#include <uapi/linux_abi.h>
#define LINUX_MAX_PATH 256
typedef int process_t;
typedef struct {uint64_t rdi,rsi,rdx,r10;} frame_t;
static int path_calls,resolve_calls,rename_calls;
static int path_failure,resolve_failure,rename_result;
static int64_t old_fd,new_fd;
static const char *old_name="/tmp/profile.new",*new_name="/tmp/Preferences";
static kstatus_t linux_path_from_user(process_t *p,int64_t fd,uint64_t pointer,char *out){
 assert(p);
 ++path_calls;
 assert(fd==(path_calls==1?old_fd:new_fd));
 if(path_failure==path_calls)return K_EBADF;
 if(!pointer)return K_EFAULT;
 strcpy(out,(const char*)(uintptr_t)pointer);return K_OK;
}
static kstatus_t vfs_resolve_path_kernel(const char *path,bool follow,bool missing,char *out){
 ++resolve_calls;assert(!follow&&missing==(resolve_calls==2));
 if(resolve_failure==resolve_calls)return K_ENOENT;
 strcpy(out,path);return K_OK;
}
static kstatus_t vfs_rename_kernel(const char *old_path,const char *new_path){
 ++rename_calls;assert(!strcmp(old_path,old_name)&&!strcmp(new_path,new_name));
 return rename_result;
}
@HELPER@
static int64_t dispatch(process_t *process,int number,frame_t *frame){
 switch(number){@DISPATCH@ default:return K_ENOSYS;}
}
static void reset(void){path_calls=resolve_calls=rename_calls=0;}
int main(void){
 process_t p=1;old_fd=new_fd=LINUX_AT_FDCWD;
 frame_t frame={.rdi=(uintptr_t)old_name,.rsi=(uintptr_t)new_name};
 assert(dispatch(&p,LINUX_SYS_RENAME,&frame)==K_OK&&rename_calls==1);
 reset();old_fd=42;new_fd=43;
 frame=(frame_t){.rdi=42,.rsi=(uintptr_t)old_name,.rdx=43,.r10=(uintptr_t)new_name};
 assert(dispatch(&p,LINUX_SYS_RENAMEAT,&frame)==K_OK&&rename_calls==1);
 for(int failure=1;failure<=2;++failure){
  reset();path_failure=failure;
  assert(dispatch(&p,LINUX_SYS_RENAMEAT,&frame)==K_EBADF&&!rename_calls&&!resolve_calls);
 }
 path_failure=0;
 for(int failure=1;failure<=2;++failure){
  reset();resolve_failure=failure;
  assert(dispatch(&p,LINUX_SYS_RENAMEAT,&frame)==K_ENOENT&&!rename_calls);
 }
 resolve_failure=0;reset();rename_result=K_EXDEV;
 assert(dispatch(&p,LINUX_SYS_RENAMEAT,&frame)==K_EXDEV&&rename_calls==1);
 reset();old_name="/";frame.rsi=(uintptr_t)old_name;
 assert(dispatch(&p,LINUX_SYS_RENAMEAT,&frame)==K_EBUSY&&!rename_calls);
 reset();old_name="/tmp/profile.new";new_name="/";
 frame.rsi=(uintptr_t)old_name;frame.r10=(uintptr_t)new_name;
 assert(dispatch(&p,LINUX_SYS_RENAMEAT,&frame)==K_EBUSY&&!rename_calls);
 reset();frame.rsi=0;
 assert(dispatch(&p,LINUX_SYS_RENAMEAT,&frame)==K_EFAULT&&!rename_calls);
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory) / 'rename.c'
    path.write_text(fixture.replace('@HELPER@', helper).replace('@DISPATCH@', dispatch))
    binary = Path(directory) / 'rename'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(root/'include'), str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('Linux rename/renameat: actual dispatch, dirfds, nofollow leaf, missing destination and failures passed')
