"""Production display once-init publication and read-only steady-state path."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/drivers/display/core.c').read_text()
function = source[source.index('static bool display_initialize('):
                  source.index('static void display_lock(')]
function = function.replace('atomic_compare_exchange_strong_explicit(&g_display_init_state',
                            'counted_cas(&g_display_init_state')
code = r'''
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <kernel/spinlock.h>
static struct {spinlock_t lock;unsigned width,height;bool pending_commit;} g_display;
static atomic_uint g_display_init_state,attempts;
static bool parallel;
static bool counted_cas(atomic_uint *p,unsigned *expected,unsigned desired,
                        memory_order success,memory_order failure) {
    atomic_fetch_add(&attempts,1);
    if(parallel)while(atomic_load(&attempts)<8)sched_yield();
    return atomic_compare_exchange_strong_explicit(p,expected,desired,success,failure);
}
''' + function + r'''
static void *initializer(void *unused) {
    (void)unused;assert(display_initialize());
    assert(atomic_load(&g_display_init_state)==2 && !atomic_load(&g_display.lock.state));
    return 0;
}
static void *query(void *unused) {
    (void)unused;
    for(unsigned i=0;i<10000;++i) {
        assert(display_initialize());
        assert(g_display.width==1920 && g_display.height==1080 && g_display.pending_commit);
        assert(atomic_load(&g_display.lock.state)==1);
    }
    return 0;
}
int main(void) {
    pthread_t threads[8];parallel=true;
    atomic_init(&g_display.lock.state,1);
    for(unsigned i=0;i<8;++i)assert(!pthread_create(&threads[i],0,initializer,0));
    for(unsigned i=0;i<8;++i)assert(!pthread_join(threads[i],0));
    parallel=false;assert(atomic_load(&attempts)==8);
    g_display.width=1920;g_display.height=1080;g_display.pending_commit=true;
    spinlock_lock(&g_display.lock);
    for(unsigned i=0;i<8;++i)assert(!pthread_create(&threads[i],0,query,0));
    for(unsigned i=0;i<8;++i)assert(!pthread_join(threads[i],0));
    assert(atomic_load(&attempts)==8 && atomic_load(&g_display.lock.state)==1);
    spinlock_unlock(&g_display.lock);
    assert(display_initialize() && atomic_load(&attempts)==8);
    puts("Display initialization: eight competing initializers, ready publication, 80000 repeated calls with zero extra CAS and preserved held lock/output/pending state passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-display-init-') as directory:
    binary = str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-x','c','-','-o',binary],
                   input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=20)
    fast_path = '    if (atomic_load_explicit(&g_display_init_state, memory_order_acquire) == 2U) return true;\n'
    assert fast_path in code
    previous = code.replace(fast_path, '')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-x','c','-','-o',binary],
                   input=previous,text=True,check=True)
    failure = subprocess.run([binary],text=True,capture_output=True,timeout=20)
    assert failure.returncode != 0 and 'attempts' in failure.stderr
    print('Regression control: removing display ready fast path fails the zero-extra-CAS gate')
