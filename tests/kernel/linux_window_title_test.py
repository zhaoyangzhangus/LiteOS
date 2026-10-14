"""Execute the actual Linux window ioctl title extension with user-copy faults."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/syscall/linux.c').read_text()
start = source.index('static kstatus_t linux_window_ioctl(')
end = source.index('{',start)+1
depth = 1
while depth:
    depth += (source[end]=='{')-(source[end]=='}')
    end += 1
body = source[start:end]
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <uapi/linux_abi.h>
#include <uapi/window_state.h>
#define __user
typedef int process_t,window_server_window_t,kstatus_t;
enum {K_OK=0,K_EINVAL=-22,LINUX_ENOTTY=-25,LINUX_EFAULT=-14};
static bool copy_fault;
static unsigned copies,titles;
static char received[32];
static process_t owner;
static window_server_window_t window;
static int window_server_geometry_owned(process_t *p,window_server_window_t *w,int32_t *x,int32_t *y,uint32_t *width,uint32_t *height,uint32_t *maximized){
 assert(p==&owner&&w==&window);*x=241;*y=176;*width=640;*height=360;*maximized=1;return 0;
}
static unsigned decoration_calls;
static int window_server_set_decorations_owned(process_t *p,window_server_window_t *w,unsigned mode){
 assert(p==&owner&&w==&window&&mode<=1);++decoration_calls;return -1;
}
static bool linux_user_pointer(uint64_t a,unsigned n){return a>4096&&a<0x800000000000ULL&&n==32;}
static int copy_from_user(void *d,const void *s,unsigned n){++copies;if(copy_fault)return -14;memcpy(d,s,n);return 0;}
static int linux_copy_status(int s){return s;}
static unsigned window_server_window_identifier(window_server_window_t *w){assert(w==&window);return 7;}
static int window_server_title_owned(process_t *p,unsigned id,const char t[32]){
 assert(p==&owner&&id==7);++titles;if(!memchr(t,0,32))return -22;memcpy(received,t,32);return 0;
}
#define window_server_present(...) 0
#define window_server_move(...) 0
#define window_server_resize(...) 0
#define window_server_close_owned(...) 0
#define window_server_set_minimized_owned(...) 0
#define window_server_capture_owned(...) 0
#define window_server_set_maximized_owned(...) 0
#define window_server_set_fullscreen_owned(...) 0
#define window_server_begin_interactive_owned(...) 0
#define window_server_set_visibility_owned(...) 0
#define WINDOW_SERVER_SHOW_INACTIVE 2
''' + body + r'''
static int invoke(linux_window_command_t *c){return linux_window_ioctl(&owner,&window,LINUX_WINDOW_IOCTL_COMMIT,c,sizeof(*c));}
int main(void){
 char title[32]={"Chrome"};
 linux_window_command_t c={.operation=LINUX_WINDOW_OPERATION_TITLE,.address=(uintptr_t)title,.length=32};
 assert(!invoke(&c)&&titles==1&&copies==1&&!strcmp(received,"Chrome"));
 c.length=31;assert(invoke(&c)==-22&&copies==1);c.length=32;
 c.address=0;assert(invoke(&c)==-14&&copies==1);c.address=(uintptr_t)title;
 copy_fault=true;assert(invoke(&c)==-14&&titles==1);copy_fault=false;
 memset(title,'x',32);assert(invoke(&c)==-22&&titles==2);
 c.x=1;assert(invoke(&c)==-22);c.x=0;
 c.y=1;assert(invoke(&c)==-22);c.y=0;
 c.width=1;assert(invoke(&c)==-22);c.width=0;
 c.height=1;assert(invoke(&c)==-22);c.height=0;
 c.stride=1;assert(invoke(&c)==-22);c.stride=0;
 c.reserved=1;assert(invoke(&c)==-25);c.reserved=0;
 c.reserved2=1;assert(invoke(&c)==-25);c.reserved2=0;
 assert(linux_window_ioctl(&owner,&window,0,&c,sizeof(c))==-25);
 assert(linux_window_ioctl(&owner,&window,LINUX_WINDOW_IOCTL_COMMIT,&c,sizeof(c)-1)==-25);
 c=(linux_window_command_t){.operation=LINUX_WINDOW_OPERATION_DECORATIONS};
 assert(invoke(&c)==-1&&decoration_calls==1);c.width=1;
 assert(invoke(&c)==-1&&decoration_calls==2);c.width=2;assert(invoke(&c)==-22);c.width=0;
 c.address=(uintptr_t)title;assert(invoke(&c)==-22&&decoration_calls==2);c.address=0;
 c.length=1;assert(invoke(&c)==-22);c.length=0;
 c.stride=1;assert(invoke(&c)==-22);c.stride=0;
 c.height=1;assert(invoke(&c)==-22);c.height=0;
 c.x=1;assert(invoke(&c)==-22);c.x=0;
 c.y=-1;assert(invoke(&c)==-22);
 c=(linux_window_command_t){.operation=LINUX_WINDOW_OPERATION_GEOMETRY};
 assert(!invoke(&c)&&c.x==241&&c.y==176&&c.width==640&&c.height==360&&c.stride==1);
 assert(invoke(&c)==-22); // query outputs cannot be replayed as input payload
 c=(linux_window_command_t){.operation=LINUX_WINDOW_OPERATION_GEOMETRY,.address=(uintptr_t)title};
 assert(invoke(&c)==-22);c.address=0;c.length=32;assert(invoke(&c)==-22);
 c=(linux_window_command_t){.operation=LINUX_WINDOW_OPERATION_FULLSCREEN};
 assert(!invoke(&c));c.width=1;assert(!invoke(&c));c.width=2;assert(invoke(&c)==-22);c.width=0;
 c.x=1;assert(invoke(&c)==-22);c.x=0;c.y=-1;assert(invoke(&c)==-22);c.y=0;
 c.height=1;assert(invoke(&c)==-22);c.height=0;c.stride=1;assert(invoke(&c)==-22);c.stride=0;
 c.address=1;assert(invoke(&c)==-22);c.address=0;c.length=1;assert(invoke(&c)==-22);
}
'''
with tempfile.TemporaryDirectory(prefix='linux-window-title-') as directory:
    base = Path(directory)
    (base/'test.c').write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root/'include'),str(base/'test.c'),'-o',str(base/'test')],check=True)
    subprocess.run([str(base/'test')],check=True)
print('Linux window title ioctl: packet bounds, payload validation, copy faults and metadata dispatch passed')
