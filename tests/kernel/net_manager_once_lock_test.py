"""Production manager initialization and state lock, with no hardware poll."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/net/manager.c').read_text()
prefix=source[:source.index('kstatus_t net_manager_subscribe(')]
queries=source[source.index('bool net_manager_get_status('):]
prefix=prefix.replace('atomic_compare_exchange_strong_explicit(&g_net_manager_init_state',
                      'counted_cas(&g_net_manager_init_state')
prefix=prefix.replace('memset(memory,', 'liteos_memset(memory,')
prefix=prefix.replace('memmove(destination,', 'liteos_memmove(destination,')
code=r'''
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
void *liteos_memset(void *,int,size_t);
void *liteos_memmove(void *,const void *,size_t);
static atomic_uint attempts,polls;
static bool counted_cas(atomic_uint *p,unsigned *expected,unsigned value,
                        memory_order success,memory_order failure) {
    atomic_fetch_add(&attempts,1);
    while(atomic_load(&attempts)<8)sched_yield();
    return atomic_compare_exchange_strong_explicit(p,expected,value,success,failure);
}
''' + prefix + queries + r'''
void net_manager_poll(void) {atomic_fetch_add(&polls,1);}
static void *initialize(void *unused) {
    (void)unused;assert(net_manager_init());
    net_manager_status_t expected={.initialized=true};
    assert(!memcmp(&g_net_manager,&expected,sizeof(expected)));
    assert(!atomic_load(&g_net_manager_lock.state) && g_net_manager_poll_divider==63);
    return NULL;
}
static uint64_t counter,published;
static void *worker(void *unused) {
    (void)unused;
    for(unsigned i=0;i<20000;++i) {
        net_manager_lock();assert(counter==published);
        ++counter;published=counter;net_manager_unlock();
    }
    return NULL;
}
int main(void) {
    unsigned memory_cases=0;
    for(unsigned length=0;length<=256;++length)
    for(unsigned from=0;from<16;++from)for(unsigned to=0;to<16;++to) {
        unsigned char actual[320],expected[320];
        for(unsigned i=0;i<320;++i)actual[i]=expected[i]=(unsigned char)(i*17+length);
        memmove(expected+32+to,expected+32+from,length);
        net_manager_copy(actual+32+to,actual+32+from,length);
        assert(!memcmp(actual,expected,sizeof(actual)));
        memset(expected+32+to,0,length);net_manager_zero(actual+32+to,length);
        assert(!memcmp(actual,expected,sizeof(actual)));++memory_cases;
    }
    net_manager_zero(NULL,0);net_manager_copy(NULL,NULL,0);
    assert(memory_cases==65792);
    net_manager_zero(&g_net_manager,sizeof(g_net_manager));
    memset(&g_net_manager,0x75,sizeof(g_net_manager));
    atomic_store(&g_net_manager_lock.state,17);
    pthread_t threads[8];
    for(unsigned i=0;i<8;++i)assert(!pthread_create(&threads[i],NULL,initialize,NULL));
    for(unsigned i=0;i<8;++i)assert(!pthread_join(threads[i],NULL));
    assert(atomic_load(&attempts)==8 && atomic_load(&polls)==1);
    for(unsigned bits=0;bits<8;++bits) {
        net_manager_lock();
        g_net_manager.hardware_present=(bits&1)!=0;
        g_net_manager.link_up=(bits&2)!=0;
        g_net_manager.ipv6_configured=(bits&4)!=0;
        net_manager_unlock();
        assert(net_manager_ready()==((bits&3)==3));
        assert(net_manager_self_test()==((bits&1)!=0 || (bits&2)==0));
        struct {unsigned char before[16];net_manager_status_t status;unsigned char after[16];} out;
        memset(&out,0x75,sizeof(out));
        assert(net_manager_get_status(&out.status));
        assert(!memcmp(&out.status,&g_net_manager,sizeof(out.status)));
        for(unsigned i=0;i<16;++i)assert(out.before[i]==0x75 && out.after[i]==0x75);
        assert(!net_manager_get_status(NULL));
        assert(!atomic_load(&g_net_manager_lock.state));
    }
    g_net_manager.link_transitions=123;g_net_manager_poll_divider=42;
    for(unsigned i=0;i<100000;++i)assert(net_manager_init());
    assert(atomic_load(&attempts)==8 && atomic_load(&polls)==1);
    assert(g_net_manager.link_transitions==123 && g_net_manager_poll_divider==42);
    net_manager_status_t copy;net_manager_copy(&copy,&g_net_manager,sizeof(copy));
    assert(!memcmp(&copy,&g_net_manager,sizeof(copy)));
    for(unsigned i=0;i<4;++i)assert(!pthread_create(&threads[i],NULL,worker,NULL));
    for(unsigned i=0;i<4;++i)assert(!pthread_join(threads[i],NULL));
    assert(counter==80000 && published==counter && !atomic_load(&g_net_manager_lock.state));
    puts("Net manager: eight concurrent initializers, one initial poll, 100000 no-CAS/no-reset calls and 80000 locked publication operations PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-net-manager-') as directory:
    binary=str(Path(directory)/'test')
    obj=str(Path(directory)/'string.o')
    subprocess.run(['cc','-std=c11','-O2','-fno-builtin','-mno-sse','-mno-mmx',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined','-fno-pie',
                    '-Dmemset=liteos_memset','-Dmemmove=liteos_memmove',
                    '-Dmemcpy=liteos_memcpy','-Dmemcmp=liteos_memcmp',
                    '-c',str(root/'kernel/string.c'),'-o',obj],check=True)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-Wno-unused-variable','-pthread','-I'+str(root/'include'),
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x','c','-','-x','none',obj,'-o',binary],input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
