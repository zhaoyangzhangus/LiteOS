"""Production service manager: bounded pointer lookup and concurrent lifecycle."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/service.c').read_text()
code = r'''
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdatomic.h>
static atomic_uint attempts;
static _Bool parallel;
static _Bool counted_cas(atomic_uint *p,unsigned *expected,unsigned value,
                        memory_order success,memory_order failure) {
 atomic_fetch_add(&attempts,1);
 if(parallel)while(atomic_load(&attempts)<8)sched_yield();
 return atomic_compare_exchange_strong_explicit(p,expected,value,success,failure);
}
''' + source.replace('atomic_compare_exchange_strong_explicit(&g_services.init_state',
                     'counted_cas(&g_services.init_state') + r'''
typedef struct {service_t *service;atomic_uint starts,stops;} context_t;
static context_t contexts[SERVICE_MANAGER_MAX_SERVICES];
static kstatus_t start(void *opaque) {
 context_t *ctx=opaque;
 assert(service_get_state(ctx->service)==SERVICE_STARTING);atomic_fetch_add(&ctx->starts,1);return K_OK;
}
static void stop(void *opaque) {
 context_t *ctx=opaque;
 assert(service_get_state(ctx->service)==SERVICE_STOPPING);atomic_fetch_add(&ctx->stops,1);
}
static void check_pointers(void) {
 uintptr_t base=(uintptr_t)g_services.services;
 for(size_t offset=0;offset<sizeof(g_services.services);++offset) {
  bool expected=offset%sizeof(service_t)==0 && g_services.services[offset/sizeof(service_t)].registered;
  assert(service_known_locked((service_t*)(base+offset))==expected);
 }
 uintptr_t invalid[]={0,1,base-1,base+sizeof(g_services.services),UINTPTR_MAX};
 for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
  service_t *service=(void*)invalid[i];
  assert(!service_known_locked(service));
  assert(service_start(service)==K_ENOENT && service_stop(service)==K_ENOENT);
  assert(service_mark_failed(service)==K_ENOENT && service_unregister(service)==K_EBUSY);
  assert(service_get_state(service)==SERVICE_FAILED);
 }
}
static void *initializer(void *unused) {
 (void)unused;assert(service_manager_init());
 for(unsigned i=0;i<SERVICE_MANAGER_MAX_SERVICES;++i)
  assert(!g_services.services[i].registered && atomic_load(&g_services.services[i].state)==SERVICE_STOPPED);
 return 0;
}
static void *worker(void *opaque) {
 context_t *ctx=opaque;
 for(unsigned i=0;i<1000;++i) {
  assert(service_start(ctx->service)==K_OK);
  assert(service_start(ctx->service)==K_EBUSY);
  assert(service_unregister(ctx->service)==K_EBUSY);
  assert(service_stop(ctx->service)==K_OK && service_stop(ctx->service)==K_OK);
 }
 return 0;
}
int main(void) {
 pthread_t threads[8];parallel=true;
 for(unsigned i=0;i<SERVICE_MANAGER_MAX_SERVICES;++i) {
  g_services.services[i].registered=true;atomic_init(&g_services.services[i].state,SERVICE_ACTIVE);
 }
 for(unsigned i=0;i<8;++i)assert(!pthread_create(&threads[i],0,initializer,0));
 for(unsigned i=0;i<8;++i)assert(!pthread_join(threads[i],0));
 parallel=false;assert(atomic_load(&attempts)==8 && service_manager_self_test());
 for(unsigned i=0;i<SERVICE_MANAGER_MAX_SERVICES;++i)
  assert(service_register("test",start,stop,&contexts[i],2,&contexts[i].service)==K_OK);
 check_pointers();service_t *unchanged=(void*)1;
 assert(service_register("full",start,stop,0,0,&unchanged)==K_ENOMEM && unchanged==(void*)1);
 for(unsigned i=0;i<SERVICE_MANAGER_MAX_SERVICES;++i) {
  assert(service_start(contexts[i].service)==K_OK);
  assert(service_mark_failed(contexts[i].service)==K_OK);
 }
 assert(!service_recover_failed(0));
 unsigned recovered=0;while(recovered<SERVICE_MANAGER_MAX_SERVICES) {
  unsigned n=service_recover_failed(3);assert(n && n<=3);recovered+=n;
 }
 assert(!service_recover_failed(1));
 for(unsigned i=0;i<SERVICE_MANAGER_MAX_SERVICES;++i)assert(service_stop(contexts[i].service)==K_OK);
 for(unsigned i=0;i<4;++i)assert(!pthread_create(&threads[i],0,worker,&contexts[i]));
 for(unsigned i=0;i<4;++i)assert(!pthread_join(threads[i],0));
 for(unsigned i=0;i<SERVICE_MANAGER_MAX_SERVICES;i+=3)assert(service_unregister(contexts[i].service)==K_OK);
 check_pointers();
 for(unsigned i=0;i<SERVICE_MANAGER_MAX_SERVICES;++i)if(i%3)assert(service_unregister(contexts[i].service)==K_OK);
 check_pointers();
 assert(atomic_load(&attempts)==8 && !atomic_load(&g_services.lock.state));
 puts("Services: eight concurrent initializers, production recovery self-test, every interior-pointer offset/bounds, 32-slot exhaustion, bounded recovery, 4000 concurrent start/stop cycles and initialized path without CAS passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-service-') as directory:
    binary = str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-x','c','-','-o',binary],
                   input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
