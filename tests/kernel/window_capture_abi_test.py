"""Exercise the actual capture ioctl branch and payload layout validation."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/syscall/linux.c').read_text()
start=source.index('    case LINUX_WINDOW_OPERATION_CAPTURE:')
end=source.index('    case ',start+len('    case LINUX_WINDOW_OPERATION_CAPTURE:'))
branch=source[start:end]
program=r'''
#include <assert.h>
#include <stddef.h>
#include "uapi/linux_abi.h"
#include "uapi/window_state.h"
#define K_EINVAL -22
static unsigned observed,calls;
static int window_server_capture_owned(void* p,void* w,unsigned capture){
 assert(p && w);observed=capture;++calls;return -16;
}
static int dispatch(linux_window_command_t* command){
 void *process=command,*window=command;
 switch(command->operation){
'''+branch+r'''
 default:return K_EINVAL;
 }
}
int main(void){
 _Static_assert(sizeof(linux_window_command_t)==48,"stable layout");
 _Static_assert(offsetof(linux_window_command_t,address)==32,"stable offset");
 linux_window_command_t c={.operation=LINUX_WINDOW_OPERATION_CAPTURE};
 assert(dispatch(&c)==-16 && observed==0 && calls==1);
 c.width=1;assert(dispatch(&c)==-16 && observed==1 && calls==2);
 c.width=2;assert(dispatch(&c)==K_EINVAL);c.width=1;
 c.height=1;assert(dispatch(&c)==K_EINVAL);c.height=0;
 c.x=-1;assert(dispatch(&c)==K_EINVAL);c.x=0;
 c.y=1;assert(dispatch(&c)==K_EINVAL);c.y=0;
 c.stride=1;assert(dispatch(&c)==K_EINVAL);c.stride=0;
 c.address=1;assert(dispatch(&c)==K_EINVAL);c.address=0;
 c.length=1;assert(dispatch(&c)==K_EINVAL);assert(calls==2);
}
'''
with tempfile.TemporaryDirectory() as directory:
    path=Path(directory);(path/'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(root/'include'),str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('Capture ABI: layout, acquire/release, propagated failure and malformed payloads passed')
