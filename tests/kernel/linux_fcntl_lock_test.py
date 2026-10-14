"""Execute the actual flock ABI helper against the real record-lock manager."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/syscall/linux.c').read_text()
helper = source[source.index('typedef struct linux_lock_wait {'):
                source.index('static int64_t linux_fcntl(process_t *process,')]
fixture = r'''
#include <assert.h>
#include <string.h>
#include <kernel/file_lock.h>
#include <uapi/linux_abi.h>
#include <uapi/wait.h>
#define __user
#define LINUX_FD_FILE 1
typedef struct { file_t *file; void *description; int kind; } linux_fd_entry_t;
typedef struct { uint64_t pid; struct { spinlock_t lock; linux_fd_entry_t entries[8]; } linux_fds; } process_t;
static int copy_failure,wait_mode,wait_calls;
static unsigned wakes;
static size_t pins;
static process_t *active;
void object_get(void *p) { assert(p); ++pins; }
void object_put(void *p) { assert(p && pins); --pins; }
uint32_t wake_all(wait_queue_t *q) { assert(q); ++wakes; return 0; }
static kstatus_t linux_copy_status(kstatus_t s) { return s; }
static kstatus_t copy_from_user(void *dst, const void *src, size_t n) {
 if(!src || copy_failure==1)return K_EFAULT;memcpy(dst,src,n);return K_OK;
}
static kstatus_t copy_to_user(void *dst, const void *src, size_t n) {
 if(!dst || copy_failure==2)return K_EFAULT;memcpy(dst,src,n);return K_OK;
}
kstatus_t vfs_seek(file_t *f,int64_t offset,uint32_t whence,uint64_t *base) {
 assert(offset==0 && whence==1);*base=f->position;return K_OK;
}
kstatus_t vfs_file_stat(file_t *f,os_file_info_t *info) { info->size=f->vnode->size;return K_OK; }
kstatus_t wait_on_queue(wait_queue_t *q,bool (*predicate)(void *),void *ctx,uint64_t timeout) {
 assert(!atomic_load(&active->linux_fds.lock.state) && timeout==OS_WAIT_INFINITE);
 assert(!predicate(ctx));++wait_calls;
 vnode_t *n=active->linux_fds.entries[3].file->vnode;
 assert(q==&n->record_lock_waitq);
 if(wait_mode==1) {
  file_lock_release_file(n,active->pid);
  active->linux_fds.entries[3]=(linux_fd_entry_t){0};
 } else file_lock_release_file(n,2);
 assert(predicate(ctx));return K_OK;
}
@HELPER@
static int64_t call(process_t *p,const linux_fd_entry_t *e,int cmd,linux_flock_t *lock) {
 return linux_fcntl_lock(p,3,e,cmd,(uintptr_t)lock);
}
int main(void) {
 vnode_t n={.mode=0100600,.size=100};
 file_t f={.vnode=&n,.flags=VFS_OPEN_READ|VFS_OPEN_WRITE,.position=20};
 linux_fd_entry_t e={&f,&f,LINUX_FD_FILE};
 process_t p={.pid=1};p.linux_fds.entries[3]=e;active=&p;
 linux_flock_t lock={.type=LINUX_F_WRLCK,.whence=1,.start=5,.length=-10};
 assert(call(&p,&e,LINUX_F_SETLK,&lock)==K_OK);
 file_lock_range_t request={2,15,24,LINUX_F_WRLCK},found;
 assert(file_lock_query(&n,&request,&found)==K_OK && found.owner==1 && found.first==15 && found.last==24);
 lock=(linux_flock_t){.type=LINUX_F_UNLCK};
 assert(call(&p,&e,LINUX_F_SETLK,&lock)==K_OK && !pins);
 request=(file_lock_range_t){2,95,99,LINUX_F_WRLCK};
 assert(file_lock_set(&n,&request)==K_OK);
 lock=(linux_flock_t){.type=LINUX_F_WRLCK,.whence=2,.start=-5,.length=5};
 assert(call(&p,&e,LINUX_F_GETLK,&lock)==K_OK && lock.pid==2 && lock.start==95 && lock.length==5 && lock.whence==0);
 assert(call(&p,&e,LINUX_F_SETLK,&lock)==K_EAGAIN);
 copy_failure=2;assert(call(&p,&e,LINUX_F_GETLK,&lock)==K_EFAULT);copy_failure=0;
 assert(call(&p,&e,LINUX_F_SETLKW,&lock)==K_OK && wait_calls==1);
 file_lock_release_owner(1);
 request=(file_lock_range_t){2,0,INT64_MAX,LINUX_F_WRLCK};assert(file_lock_set(&n,&request)==K_OK);
 lock=(linux_flock_t){.type=LINUX_F_WRLCK};wait_mode=1;
 assert(call(&p,&e,LINUX_F_SETLKW,&lock)==K_EBADF && wait_calls==2);
 p.linux_fds.entries[3]=e;file_lock_release_owner(2);
 f.flags=VFS_OPEN_READ;assert(call(&p,&e,LINUX_F_SETLK,&lock)==K_EBADF);
 lock.type=LINUX_F_RDLCK;assert(call(&p,&e,LINUX_F_SETLK,&lock)==K_OK);
 unsigned saved_wakes=wakes,sequence=atomic_load(&n.record_lock_sequence);
 for(int i=0;i<1000;++i)assert(call(&p,&e,LINUX_F_SETLK,&lock)==K_OK);
 assert(wakes==saved_wakes && atomic_load(&n.record_lock_sequence)==sequence);
 lock.type=LINUX_F_UNLCK;
 assert(call(&p,&e,LINUX_F_SETLK,&lock)==K_OK && wakes==saved_wakes+1);
 saved_wakes=wakes;
 assert(call(&p,&e,LINUX_F_SETLK,&lock)==K_OK && wakes==saved_wakes);
 file_lock_release_owner(1);f.flags=VFS_OPEN_READ|VFS_OPEN_WRITE;
 lock.whence=3;assert(call(&p,&e,LINUX_F_GETLK,&lock)==K_EINVAL);
 lock.whence=0;lock.start=-1;assert(call(&p,&e,LINUX_F_SETLK,&lock)==K_EINVAL);
 lock.start=INT64_MAX;lock.length=2;assert(call(&p,&e,LINUX_F_SETLK,&lock)==K_EOVERFLOW);
 lock.start=0;lock.length=INT64_MIN;assert(call(&p,&e,LINUX_F_SETLK,&lock)==K_EINVAL);
 lock.length=0;lock.type=8;assert(call(&p,&e,LINUX_F_SETLK,&lock)==K_EINVAL);
 lock.type=LINUX_F_UNLCK;assert(call(&p,&e,LINUX_F_GETLK,&lock)==K_EINVAL);
 n.mode=0040700;assert(call(&p,&e,LINUX_F_SETLK,&lock)==K_EBADF);n.mode=0100600;
 assert(call(&p,&e,LINUX_F_SETLK,NULL)==K_EFAULT);
 copy_failure=1;assert(call(&p,&e,LINUX_F_SETLK,&lock)==K_EFAULT);
 assert(!pins);
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path/'test.c').write_text(fixture.replace('@HELPER@', helper))
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-Wno-misleading-indentation', '-fsanitize=address,undefined',
        '-fno-pie', '-no-pie', '-I', str(root/'include'), str(path/'test.c'),
        str(root/'kernel/fs/vfs/record_lock.c'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True, timeout=10)
print('Actual fcntl lock ABI: relative/negative ranges, overflow, rights, query/copy faults, blocking wake and concurrent close passed')
