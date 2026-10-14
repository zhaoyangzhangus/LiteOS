"""Run actual exit/cancel formatters and exec progress quota/debug state."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
for name in ('exec.c', 'exit.c', 'thread.c'):
    assert 'LITEOS_' not in (ROOT / 'kernel/process' / name).read_text(), name


def function_end(source, start):
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return end


def raw_diagnostics(name, start, end, sink):
    source = (ROOT / 'kernel/process' / name).read_text()
    source = source[source.index(start):source.index(end)]
    begin = source.index('static void ' + sink + '(')
    return source[:begin] + f'static void {sink}(char c) {{ capture(c); }}\n' + source[function_end(source, begin):]


raw = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/process.h>
#include <kernel/sched.h>
static char output[1024];
static size_t used;
static thread_t *current;
thread_t *sched_current_thread(void) { return current; }
static void capture(char c) { assert(used+1 < sizeof(output)); output[used++]=c; }
static void reset(void) { memset(output,0,sizeof(output)); used=0; }
'''
raw += raw_diagnostics('exit.c', 'static atomic_flag g_process_exit_diag_lock',
                       'bool process_publish_thread_exit', 'process_exit_diag_char')
raw += raw_diagnostics('thread.c', 'static atomic_flag g_thread_cancel_diag_lock',
                       '\n#endif', 'thread_cancel_diag_char')
raw += r'''
int main(void) {
    process_t process={.pid=UINT64_MAX};
    thread_t target={.tid=0x123456789abcdef0,.process=&process}; current=&target;
    process_exit_diag(&process,&target,INT64_MIN,UINT32_MAX);
    assert(!strcmp(output,"OS_PROCESS_EXIT pid=0xffffffffffffffff tid=0x123456789abcdef0 status=0x8000000000000000 signal=0x00000000ffffffff\r\n"));
    reset(); process_exit_diag(NULL,NULL,-1,0);
    assert(!strcmp(output,"OS_PROCESS_EXIT pid=0x0000000000000000 tid=0x0000000000000000 status=0xffffffffffffffff signal=0x0000000000000000\r\n"));
    reset(); thread_cancel_diag(&target,K_EIO);
    assert(!strcmp(output,"OS_THREAD_WAIT_CANCEL target_pid=ffffffffffffffff target_tid=123456789abcdef0 caller_pid=ffffffffffffffff caller_tid=123456789abcdef0 status=fffffffffffffffb\r\n"));
    current=NULL; reset(); thread_cancel_diag(NULL,0);
    assert(!strcmp(output,"OS_THREAD_WAIT_CANCEL target_pid=0000000000000000 target_tid=0000000000000000 caller_pid=0000000000000000 caller_tid=0000000000000000 status=0000000000000000\r\n"));
    target.process=NULL; current=&target; reset(); thread_cancel_diag(&target,INT64_MIN);
    assert(strstr(output,"target_pid=0000000000000000") && strstr(output,"caller_pid=0000000000000000") && strstr(output,"status=8000000000000000\r\n"));
    assert(!process_is_chrome_image(NULL) && !process_is_chrome_image(&process));
    strcpy(process.linux_exe,"/sbin/chrome"); assert(process_is_chrome_image(&process));
    strcpy(process.linux_exe,"/sbin/chrome-extra"); assert(!process_is_chrome_image(&process));
    puts("PROCESS_EXIT_CANCEL_DIAGNOSTIC_TEST_OK");
}
'''
source = (ROOT / 'kernel/process/exec.c').read_text()
debug = source[source.index('static atomic_uint g_exec_debug_stage'):source.index('typedef struct elf_exec_arguments')]
progress = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#define printf test_console_printf
''' + f'#include "{ROOT / "kernel/console_printf.c"}"\n' + r'''
#undef printf
static _Thread_local thread_t *current;
static char output[256];
static unsigned records;
thread_t *sched_current_thread(void) { return current; }
void kernel_serial_write(const char *text) { (void)text; assert(0); }
void kernel_serial_write_serial_only(const char *text) {
    assert(strlen(text)<sizeof(output)); strcpy(output,text); ++records;
}
''' + debug + r'''
#if OS_DEBUG_SERIAL
static atomic_uint accepted;
static void *worker(void *unused) {
    (void)unused;
    for (unsigned i=0;i<1000;++i)
        if (exec_debug_serial_allow()) atomic_fetch_add(&accepted,1);
    return NULL;
}
#endif
int main(void) {
    process_exec_debug_reset();
    assert(!process_exec_debug_stage() && process_exec_debug_status() == (uint32_t)K_EBUSY);
    atomic_store(&g_exec_debug_thread,17);
    process_exec_debug_mark(1); assert(!process_exec_debug_stage() && !records);
    thread_t thread={.tid=18}; current=&thread;
    process_exec_debug_mark(2); assert(!process_exec_debug_stage() && !records);
    thread.tid=17; exec_debug_progress(0xabc);
    assert(process_exec_debug_stage() == 0xabc);
    exec_debug_result(K_EIO); assert(process_exec_debug_status() == (uint32_t)K_EIO);
#if OS_DEBUG_SERIAL
    assert(records == 1 && !strcmp(output,"OS_EXEC_PROGRESS tid=17 stage=abc\r\n"));
    process_exec_debug_reset();
    pthread_t workers[8];
    for (unsigned i=0;i<8;++i) assert(!pthread_create(&workers[i],NULL,worker,NULL));
    for (unsigned i=0;i<8;++i) assert(!pthread_join(workers[i],NULL));
    assert(atomic_load(&accepted) == 128 && g_exec_debug_serial_logs == 128);
    for (unsigned i=0;i<2000;++i) assert(!exec_debug_serial_allow());
    assert(g_exec_debug_serial_logs == 128);
    atomic_store(&g_exec_debug_thread,17);
    exec_debug_progress(3); assert(process_exec_debug_stage() == 3 && records == 1);
    __atomic_store_n(&g_exec_debug_serial_logs,UINT32_MAX,__ATOMIC_RELAXED);
    assert(!exec_debug_serial_allow() && g_exec_debug_serial_logs == UINT32_MAX);
    process_exec_debug_reset(); assert(!g_exec_debug_serial_logs && exec_debug_serial_allow());
#else
    assert(!records);
#endif
    process_exec_debug_reset(); exec_debug_progress(4); assert(!process_exec_debug_stage());
    assert(!atomic_load(&g_exec_debug_thread) && process_exec_debug_status() == (uint32_t)K_EBUSY);
    puts("EXEC_PROGRESS_QUOTA_TEST_OK");
}
'''
with tempfile.TemporaryDirectory(prefix='process-diagnostics-') as temporary:
    fixture = Path(temporary) / 'test.c'; binary = Path(temporary) / 'test'
    for code,flags in ((raw,[]),(progress,['-DOS_DEBUG_SERIAL=0']),
                       (progress,['-DOS_DEBUG_SERIAL=1'])):
        fixture.write_text(code)
        subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-pthread','-fno-builtin',
                        '-fsanitize=address,undefined','-fno-pie','-no-pie',
                        '-I',str(ROOT/'include'),*flags,str(fixture),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
