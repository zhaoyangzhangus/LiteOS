"""Run the actual shell power command with a fake syscall."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'user/desktop/gshell/main.c').read_text()
start = source.index('static void command_power(')
end = source.index('static void execute_command(', start)
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <strings.h>
#include <string.h>
#define OS_SYS_SYSTEM_POWER 0x0930
static unsigned calls, messages;
static uint64_t last_action;
static const char *last_message;
static void append_output(const char *s) {++messages;last_message=s;}
static bool text_equals_ignore_case(const char *a,const char *b) {return !strcasecmp(a,b);}
static int64_t gshell_syscall_one(uint64_t n,uint64_t action) {
    assert(n==OS_SYS_SYSTEM_POWER);++calls;last_action=action;return -5;
}
''' + source[start:end] + r'''
int main(void) {
    char *args[]={"reboot","now"};
    command_power(args,2,false);assert(!calls);
    command_power(args,1,true);assert(!calls);
    command_power(args,1,false);assert(calls==1 && !last_action);
    args[0]="ShUtDoWn";
    command_power(args,1,false);assert(calls==2 && last_action==1);
    assert(messages==4 && strstr(last_message,"FAILED"));
}
'''
with tempfile.TemporaryDirectory(prefix='gshell-power-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-x', 'c', '-', '-o', binary], input=code, text=True, check=True)
    subprocess.run([binary], check=True)
print('Shell reboot/shutdown: actions, case folding, argument/background rejection and error message PASS')
