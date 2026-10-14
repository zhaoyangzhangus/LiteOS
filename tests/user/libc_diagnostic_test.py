"""Run real startup diagnostics and bounded trace admission helpers."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
for path in (ROOT / 'user/libc').rglob('*.c'):
    assert not re.search(r'LITEOS_(?:LIBC|NETDB)_[A-Z0-9_]+',path.read_text()), path


def function(source, signature):
    start=source.index(signature); opening=source.index('{',start)
    depth,end=1,opening+1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}'); end+=1
    return source[start:end]


startup=(ROOT/'user/libc/startup.c').read_text().replace('#include "libc/libc.h"','')
code=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include <uapi/syscall.h>
static jmp_buf finish;
static int thread_status, exit_status;
static unsigned stage, trace_count;
static char traces[2][64];
static char *arguments[]={"app",NULL};
static char *environment[]={"KEY=value",NULL};
int64_t __libc_linux_syscall6(uint64_t number,uint64_t text,uint64_t length,
                             uint64_t a,uint64_t b,uint64_t c,uint64_t d) {
    assert(number==OS_SYS_DEBUG_WRITE && !a && !b && !c && !d);
    assert(trace_count<2 && length==strlen((char *)(uintptr_t)text));
    assert(length<sizeof(traces[0])); strcpy(traces[trace_count++],(char *)(uintptr_t)text);
    return -5;
}
int __libc_thread_init(void) { assert(stage++==0 && trace_count==1); return thread_status; }
void __libc_init_descriptors(char **envp) { assert(stage++==1 && envp==environment); }
void __libc_init_environment_main(int argc,char **argv,char **envp) {
    assert(stage++==2 && argc==1 && argv==arguments && envp==environment);
}
int guest_main(int argc,char **argv,char **envp) {
    assert(stage++==3 && argc==1 && argv==arguments && envp==environment); return 42;
}
static void test_exit(int value) { assert(stage==4); exit_status=value; longjmp(finish,1); }
static void test_immediate_exit(int value) { assert(stage==1); exit_status=value; longjmp(finish,1); }
#define main guest_main
#define exit test_exit
#define _exit test_immediate_exit
#define __libc_start_main tested_start_main
''' + startup + r'''
#undef main
#undef exit
#undef _exit
int main(void) {
    if (!setjmp(finish)) { tested_start_main(1,arguments,environment); assert(0); }
    assert(exit_status==42 && stage==4 && trace_count==2);
    assert(!strcmp(traces[0],"OS_LIBC_STARTUP_BEGIN\n"));
    assert(!strcmp(traces[1],"OS_LIBC_THREAD_INIT_OK\n"));
    stage=trace_count=0; thread_status=-1;
    if (!setjmp(finish)) { tested_start_main(1,arguments,environment); assert(0); }
    assert(exit_status==127 && stage==1 && trace_count==2);
    assert(!strcmp(traces[1],"OS_LIBC_THREAD_INIT_FAIL\n"));
    puts("LIBC_STARTUP_DIAGNOSTIC_TEST_OK");
}
'''
syscalls=(ROOT/'user/libc/syscall.c').read_text()
budget=function(syscalls,'static bool libc_trace_budget(') + function(syscalls,'static bool libc_debug_budget(')
dns=function((ROOT/'user/libc/netdb.c').read_text(),'static bool netdb_dns_trace_begin(')
quotas=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>
static volatile uint32_t counter;
static uint32_t g_netdb_dns_trace_count;
static atomic_uint accepted, dns_accepted;
''' + budget + dns + r'''
static void *worker(void *unused) {
    (void)unused;
    for (unsigned i=0;i<1000;++i) {
        if (libc_debug_budget(&counter)) atomic_fetch_add(&accepted,1);
        if (netdb_dns_trace_begin()) atomic_fetch_add(&dns_accepted,1);
    }
    return NULL;
}
int main(void) {
    pthread_t workers[8];
    volatile uint32_t exhausted = UINT32_MAX;
    assert(!libc_trace_budget(&exhausted, 128U) && exhausted == UINT32_MAX);
    for (unsigned i=0;i<8;++i) assert(!pthread_create(&workers[i],NULL,worker,NULL));
    for (unsigned i=0;i<8;++i) assert(!pthread_join(workers[i],NULL));
#ifdef OS_RUNTIME_TRACE
    assert(counter==128 && atomic_load(&accepted)==128 && !libc_debug_budget(&counter));
#else
    assert(!counter && !atomic_load(&accepted) && !libc_debug_budget(&counter));
#endif
    assert(g_netdb_dns_trace_count==4 && atomic_load(&dns_accepted)==4 && !netdb_dns_trace_begin());
    counter=g_netdb_dns_trace_count=UINT32_MAX;
    assert(!libc_debug_budget(&counter) && !netdb_dns_trace_begin());
    assert(counter==UINT32_MAX && g_netdb_dns_trace_count==UINT32_MAX);
    counter=g_netdb_dns_trace_count=0;
    assert(netdb_dns_trace_begin() && g_netdb_dns_trace_count==1);
    puts("LIBC_DIAGNOSTIC_QUOTA_TEST_OK");
}
'''
with tempfile.TemporaryDirectory(prefix='libc-diagnostics-') as temporary:
    fixture=Path(temporary)/'test.c'; binary=Path(temporary)/'test'
    for body,flags in ((code,[]),(quotas,[]),(quotas,['-DOS_RUNTIME_TRACE'])):
        fixture.write_text(body)
        subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-pthread',
                        '-fsanitize=address,undefined','-fno-pie','-no-pie',
                        '-I',str(ROOT/'include'),*flags,str(fixture),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
