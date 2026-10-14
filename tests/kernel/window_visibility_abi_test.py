"""Compile the production visibility dispatch against its actual UAPI layout."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/syscall/linux.c').read_text()
start = source.index('    case LINUX_WINDOW_OPERATION_VISIBILITY:')
end = source.index('\n    default:',start)
branch = source[start:end]
program = r'''
#include <assert.h>
#include <stddef.h>
#include "uapi/linux_abi.h"
#define K_EINVAL -22
#define WINDOW_SERVER_SHOW_INACTIVE 2U
static unsigned observed;
static int window_server_set_visibility_owned(void *p,void *w,unsigned flags) {
 assert(p && w); observed=flags; return 0;
}
static int dispatch(linux_window_command_t *command) {
 void *process=command,*window=command;
 switch(command->operation) {
''' + branch + r'''
 default: return K_EINVAL;
 }
}
int main(void) {
 _Static_assert(sizeof(linux_window_command_t)==48,"stable ABI");
 _Static_assert(offsetof(linux_window_command_t,address)==32,"stable offset");
 linux_window_command_t c={.operation=LINUX_WINDOW_OPERATION_VISIBILITY};
 assert(dispatch(&c)==0 && observed==0);
 c.width=1; assert(dispatch(&c)==0 && observed==1);
 c.height=LINUX_WINDOW_VISIBILITY_INACTIVE;
 assert(dispatch(&c)==0 && observed==3);
 c.width=0; assert(dispatch(&c)==K_EINVAL);
 c.width=2; assert(dispatch(&c)==K_EINVAL);
 c.width=1;c.height=2; assert(dispatch(&c)==K_EINVAL);
 c.height=1;c.stride=1; assert(dispatch(&c)==K_EINVAL);
 c.stride=0;c.address=1; assert(dispatch(&c)==K_EINVAL);
 c.address=0;c.length=1; assert(dispatch(&c)==K_EINVAL);
 c.length=0;c.x=1; assert(dispatch(&c)==K_EINVAL);
 c.x=0;c.y=1; assert(dispatch(&c)==K_EINVAL);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-window-abi-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I', str(root/'include'), str(path/'test.c'),
                    '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')],check=True)
print('Visibility ABI: 48-byte layout, active/inactive payload and malformed commands passed')
