"""Compile real task-manager sampling/tree/actions with a deterministic kernel."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'user/desktop/taskmgr/main.c').read_text()

def function(name):
    match = re.search(r'^static [^\n]*\b' + name + r'\([^;{}]*\)\s*\{', source, re.M)
    assert match, name
    begin = match.start()
    end = source.index('{', begin) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[begin:end]

declarations = source[source.index('typedef struct taskmgr_window {'):source.index('void __main(void)')]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <uapi/all.h>
#define TASKMGR_MAX_ENTRIES 768U
#define TASKMGR_STATUS_CAP 96U
#define TASKMGR_MAX_PROCESSES 256U
#define TASKMGR_MAX_THREADS 512U
#define TASKMGR_REFRESH_NS 1000000000ULL
#define TASKMGR_ROW_HEIGHT 34U
#define TASKMGR_TABLE_TOP TASKMGR_ROW_HEIGHT
#define TASKMGR_STATUS_HEIGHT 36U
''' + declarations + r'''
static uint64_t clock_ns=1000000000ULL;
static bool alive=true, enumeration_failure;
static bool thread_alive=true;
static unsigned kills,thread_kills;
static uint32_t ui_text_measure(const char *text) { return (uint32_t)strlen(text)*8U; }
static int64_t taskmgr_syscall6(uint64_t n,uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
 (void)a;(void)c;(void)d;(void)e;(void)f;assert(n==OS_SYS_CLOCK_GET);
 os_timespec_t *t=(void *)(uintptr_t)b;t->seconds=clock_ns/1000000000ULL;t->nanoseconds=clock_ns%1000000000ULL;return 0;
}
static int64_t taskmgr_syscall_one(uint64_t n,uint64_t p) {
 if(n==OS_SYS_PROCESS_ENUMERATE) {
  os_process_enumerate_t *r=(void *)(uintptr_t)p;
  if(enumeration_failure)return -5;
  if(r->index >= (alive?2U:1U))return -2;
  r->info.pid=r->index?10:1;r->info.state=OS_PROCESS_RUNNING;r->info.thread_count=r->index?2:1;
  strcpy(r->info.name,r->index?"Test":"System");return 0;
 }
 if(n==OS_SYS_THREAD_ENUMERATE) {
  os_thread_enumerate_t *r=(void *)(uintptr_t)p;
  if(!alive || r->index >= (thread_alive?2U:1U))return -2;
  r->info.tid=11+r->index;r->info.process_pid=10;r->info.state=OS_THREAD_READY;
  strcpy(r->info.name,"Worker");return 0;
 }
 if(n==OS_SYS_TASK_QUERY) {
  os_task_info_t *r=(void *)(uintptr_t)p;
  r->create_time_ns=123;r->cpu_count=4;r->runtime_ns=clock_ns/2;
  r->rss_bytes=12582912;r->flags=r->pid==1?OS_TASK_PROTECTED:0;return 0;
 }
 if(n==OS_SYS_THREAD_TERMINATE){
  os_thread_terminate_t *r=(void *)(uintptr_t)p;
  assert(r->pid==10 && r->tid==12 && r->create_time_ns==123);
  ++thread_kills;thread_alive=false;return 0;
 }
 assert(n==OS_SYS_PROCESS_TERMINATE);
 os_process_terminate_t *r=(void *)(uintptr_t)p;
 assert(r->pid==10 && r->create_time_ns==123);++kills;alive=false;return 0;
}
'''
for name in ('text_length','copy_text','append_text','append_u64','set_status',
             'monotonic_ns','sample_resources','entry_visible','visible_entry',
             'entry_row','visible_count','menu_width','menu_rows','menu_hit',
             'dismiss_menu','refresh_processes','end_selected_process','visible_rows'):
    program += function(name) + '\n'
program += r'''
int main(void) {
 refresh_processes();assert(g_entry_count==4 && visible_count()==2);
 assert(g_entries[1].resources.rss_bytes==12582912 && g_entries[1].cpu==0);
 g_entries[1].expanded=true;g_selection_valid=true;g_selected_entry=3;g_first_row=1;
 assert(visible_count()==4 && visible_entry(2)==2 && entry_row(3)==3);
 clock_ns+=1000000000ULL;refresh_processes();
 assert(g_entries[1].expanded && g_selected_entry==3 && g_first_row==1);
 assert(g_entries[1].cpu==12500 && g_entries[2].cpu==12500); // 50% of one core / four CPUs
 assert(g_entries[1].cpu_sample_valid);
 assert(g_next_refresh==clock_ns+1000000000ULL);
 g_window.width=1040;g_window.height=680;g_menu_x=400;g_menu_y=300;g_menu_open=true;
 assert(visible_rows()==17);
 for(unsigned height=0;height<1000;++height) {
  g_window.height=height;
  unsigned bottom=height>36?height-36:0;
  assert(visible_rows()==(bottom>34?(bottom-34)/34:0));
 }
 g_window.height=680;
 assert(menu_rows()==2 && menu_hit(410,310)==0 && menu_hit(410,344)==1);
 assert(menu_hit(399,310)==-1 && menu_hit(410,368)==-1);
 end_selected_process(true);assert(!kills && !thread_kills && g_confirm_pid==10 && g_confirm_tid==12);
 clock_ns+=1000000000ULL;refresh_processes();assert(g_confirm_pid==10 && g_selected_entry==3);
 end_selected_process(true);assert(thread_kills==1 && !kills && alive && !g_menu_open);
 g_selected_entry=1;
 end_selected_process(false);assert(!kills && g_confirm_pid==10 && !g_confirm_tid);
 end_selected_process(false);assert(kills==1 && g_entry_count==1 && !g_confirm_pid);
 end_selected_process(false);assert(kills==1 && strstr(g_status,"SYSTEM"));
 enumeration_failure=true;clock_ns+=1000000000ULL;refresh_processes();
 assert(g_next_refresh==clock_ns+1000000000ULL); // failures must not create a busy poll loop
}
'''
assert source.index('if (now >= g_next_refresh)') < source.index('request.timeout_ns = now < g_next_refresh')
with tempfile.TemporaryDirectory(prefix='system-taskmgr-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-variable',
                    '-Wno-unused-function','-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root/'include'),str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('Task manager: periodic refresh, CPU/RSS, grouping, stable selection/scroll, protected end and confirmation passed')
