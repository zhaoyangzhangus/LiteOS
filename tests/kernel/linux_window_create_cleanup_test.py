"""Actual Linux window adapter: retained rollback VM and local window ref."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/syscall/linux.c').read_text()
body = source[source.index('static int64_t linux_window_create('):
              source.index('static kstatus_t linux_window_ioctl(')]
internal = (root / 'kernel/process/internal.h').read_text()
helper = internal[internal.index('static inline vm_space_t *process_vm_reference('):]
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#define __user
#define K_OK 0
#define K_EINVAL -22
#define LINUX_EFAULT -14
#define PAGE_SIZE 4096ULL
#define X86_64_USER_TOP 0x7fffffffffffULL
#define OS_WINDOW_VISIBLE 1
#define OS_WINDOW_RESIZABLE 2
#define OS_WINDOW_CLIENT_DECORATIONS 4
#define OS_WINDOW_PREMULTIPLIED_ALPHA 8
#define OS_WINDOW_STANDARD_CAPTION_CONTROLS 16
#define OS_WINDOW_EXPLICIT_MOVE_RESIZE 64
#define OS_WINDOW_SURFACE_EXCHANGE 32
#define LINUX_FD_WINDOW 1
#define LINUX_O_NONBLOCK 1
#define LINUX_O_CLOEXEC 2
typedef int kstatus_t;
typedef struct {unsigned refs;} vm_space_t;
typedef struct {vm_space_t *vm;uint64_t pid;} process_t;
typedef struct {int x,y;unsigned width,height,flags,background;char title[32];uint64_t reserved,buffer_size,identifier,address;} linux_window_create_t;
typedef struct {unsigned refs;} window_server_window_t;
static process_t p;
static vm_space_t *expected;
static window_server_window_t *active;
static linux_window_create_t args;
static bool locked,scene,fd;
static unsigned failure,mode,stage,unmaps,closes,puts,vm_destroyed,window_destroyed,gets;
static unsigned g_linux_window_create_debug_count;
static void kernel_serial_printf_serial_only(const char *format,...){(void)format;assert(!locked);}
static bool linux_user_pointer(uint64_t a,size_t n){return a && n==sizeof(args);}
static bool x86_is_canonical(uint64_t a){return a<=X86_64_USER_TOP;}
static int copy_from_user(void *to,const void *from,size_t n){assert(from==&args);memcpy(to,from,n);return 0;}
static int64_t linux_copy_status(int s){return s;}
static void process_thread_lock(process_t *v){assert(v==&p && !locked);locked=true;}
static void process_thread_unlock(process_t *v){assert(v==&p && locked);locked=false;}
static vm_space_t *allocate(void){vm_space_t *v=calloc(1,sizeof(*v));assert(v);v->refs=1;return v;}
static void vm_space_get(vm_space_t *v){assert(locked && v->refs);++v->refs;++gets;}
static void vm_space_put(vm_space_t *v){assert(!locked);if(v && !--v->refs){++vm_destroyed;free(v);}}
@HELPER@
static void window_server_put(window_server_window_t *w){assert(!locked && w==active && w->refs);++puts;if(!--w->refs){++window_destroyed;free(w);}}
static void object_get(window_server_window_t *w){assert(!locked && w==active && w->refs);++w->refs;}
static int window_server_close_owned(process_t *v,window_server_window_t *w){assert(v==&p && w==active && w->refs);++closes;if(scene){scene=false;window_server_put(w);}return 0;}
static int linux_close(process_t *v,int64_t d){assert(v==&p && d==7);if(fd){fd=false;window_server_put(active);}return 0;}
static void inject(unsigned where){
 assert(!locked);
 if(stage!=where || !mode)return;
 if(mode<=2){
  process_thread_lock(&p);assert(p.vm==expected);p.vm=mode==1?NULL:allocate();process_thread_unlock(&p);
  vm_space_put(expected);
 } else if(where==5){linux_close(&p,7);window_server_close_owned(&p,active);}
}
static int window_server_create(process_t *v,int x,int y,unsigned w,unsigned h,unsigned f,unsigned bg,const char *title,window_server_window_t **out){
 assert(v==&p && x==10 && y==20 && w==640 && h==480 && f==1 && bg==0 && title);
 if(failure==1)return -12;
 active=calloc(1,sizeof(*active));assert(active);active->refs=2;scene=true;*out=active;return 0;
}
static uint64_t window_server_window_buffer_size(window_server_window_t *w){assert(w==active && w->refs);return 8192;}
static int window_server_map_space(window_server_window_t *w,vm_space_t *v,uint64_t requested,uint64_t *out){assert(w==active && v==expected && v->refs==2 && !requested);if(failure==2)return -12;*out=0x20000;return 0;}
static int window_server_set_owner_address(window_server_window_t *w,uint64_t address){assert(w==active && w->refs && address==0x20000);inject(2);return failure==3?-5:0;}
static int window_server_set(window_server_window_t *w,int x,int y,unsigned visible){assert(w==active && w->refs && x==10 && y==20 && visible==1);inject(3);return failure==4?-5:0;}
static int64_t linux_fd_install(process_t *v,int64_t request,void *file,window_server_window_t *w,unsigned kind,unsigned flags){
 assert(v==&p && request==-1 && !file && w==active && w->refs==3 && kind==1 && flags==3);
 inject(4);if(failure==5)return -24;fd=true;return 7;
}
static unsigned window_server_window_identifier(window_server_window_t *w){assert(w==active && w->refs);return 123;}
static int copy_to_user(void *to,const void *from,size_t n){assert(to==&args && n==sizeof(args));inject(5);assert(active->refs);if(failure==6)return -14;memcpy(to,from,n);return 0;}
static int vm_unmap(vm_space_t *v,uint64_t address,size_t size){assert(!locked && v==expected && v->refs && address==0x20000 && size==8192);++unmaps;return -5;}
@BODY@
int main(void){
 unsigned cases=0;
 for(unsigned f=0;f<=6;++f)for(unsigned m=0;m<=3;++m)for(unsigned s=2;s<=5;++s){
  failure=f;mode=m;stage=s;p.vm=allocate();expected=p.vm;active=NULL;scene=fd=locked=false;
  unmaps=closes=puts=vm_destroyed=window_destroyed=gets=0;
  args=(linux_window_create_t){.x=10,.y=20,.width=640,.height=480,.flags=1};
  int64_t want=f==1 || f==2?-12:f==3 || f==4?-5:f==5?-24:f==6?-14:7;
  assert(linux_window_create(&p,(uint64_t)(uintptr_t)&args)==want);
  assert(!locked && unmaps==(f>=3) && gets==(f!=1));
  bool reached=f==0 || (s==2?f>=3:s==3?f>=4:s==4?f>=5:f>=6);
  bool detached=(m==1 || m==2) && reached;
  assert(vm_destroyed==detached);
  if(p.vm){assert(p.vm->refs==1);vm_space_put(p.vm);p.vm=NULL;}
  assert(vm_destroyed==(detached && m==2?2:1));
  if(!f){
   assert(args.identifier==123 && args.address==0x20000 && args.buffer_size==8192);
   if(m==3 && s==5)assert(window_destroyed==1 && !scene && !fd);
   else {assert(active->refs==2 && scene && fd);window_server_close_owned(&p,active);linux_close(&p,7);}
  } else if(f!=1)assert(window_destroyed==1 && !scene && !fd);
  assert(window_destroyed==(f!=1));++cases;
 }
 assert(cases==112);return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    directory = Path(directory)
    def run(name, implementation, pin=helper, harness=fixture, debug=0):
        path = directory / (name + '.c')
        path.write_text(harness.replace('@HELPER@', pin).replace('@BODY@', implementation))
        binary = directory / name
        subprocess.run(['cc', '-std=c11', '-fsanitize=address,undefined', '-fno-pie',
                        '-no-pie', f'-DOS_DEBUG_SERIAL={debug}', str(path), '-o', str(binary)], check=True)
        return subprocess.run([str(binary)], capture_output=True, text=True)
    for debug in (0, 1):
        result = run('create'+str(debug), body, debug=debug)
        assert result.returncode == 0, result.stderr
    result = run('wrong_rollback', body.replace('vm_unmap(space,', 'vm_unmap(process->vm,'))
    assert result.returncode != 0, result.stderr
    closing = fixture.replace('unsigned m=0;', 'unsigned m=3;').replace(
        'unsigned s=2;', 'unsigned s=5;').replace('w->refs==3', 'w->refs>=2')
    result = run('missing_local', body.replace('    object_get(window);', ''), harness=closing)
    assert result.returncode != 0 and 'heap-use-after-free' in result.stderr, result.stderr
    result = run('leaked_vm', body.replace('    vm_space_put(space);', ''))
    assert result.returncode != 0, result.stderr
print('Linux window creation: 112 failure-stage/exit/exec/concurrent-close cases, exact rollback and balanced VM/window/fd ownership passed; wrong VM, missing local reference and leaked pin negatives rejected')
