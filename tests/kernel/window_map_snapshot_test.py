"""Actual window mapping pins one VM across map/populate/rollback."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/graphics/window.c').read_text()
body = source[source.index('kstatus_t window_server_map_space('):
              source.index('kstatus_t window_server_dispatch(')]
internal = (root / 'kernel/process/internal.h').read_text()
helper = internal[internal.index('static inline vm_space_t *process_vm_reference('):]
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#define K_OK 0
#define K_EINVAL -22
#define K_EIO -5
#define VM_PROT_USER 1
#define VM_PROT_READ 2
#define VM_PROT_WRITE 4
#define VM_MAP_SHARED 8
typedef int kstatus_t;
typedef uint64_t vaddr_t;
typedef struct {unsigned refs;} vm_space_t;
typedef struct {unsigned refs;} vm_object_t;
typedef struct {vm_space_t *vm;} process_t;
typedef struct {vm_object_t *vm_object;} section_t;
typedef struct {section_t *section;uint64_t buffer_size;} window_server_window_t;
static process_t p;
static vm_space_t *expected;
static bool locked;
static unsigned mode,stage,maps,populates,unmaps,destroyed;
static int map_status,populate_status;
static void process_thread_lock(process_t *v){assert(v==&p && !locked);locked=true;}
static void process_thread_unlock(process_t *v){assert(v==&p && locked);locked=false;}
static vm_space_t *allocate(void){vm_space_t *v=calloc(1,sizeof(*v));assert(v);v->refs=1;return v;}
static void vm_space_get(vm_space_t *v){assert(locked && v->refs);++v->refs;}
static void vm_space_put(vm_space_t *v){assert(!locked);if(v && !--v->refs){++destroyed;free(v);}}
@HELPER@
static void inject(vm_space_t *v,unsigned where){
 assert(!locked && v==expected);
 if(mode && stage==where){
  process_thread_lock(&p);p.vm=mode==1?NULL:allocate();process_thread_unlock(&p);
  vm_space_put(v);
 }
 assert(v->refs);
}
static void vm_object_get(vm_object_t *v){assert(!locked && v->refs==1);++v->refs;}
static void vm_object_put(vm_object_t *v){assert(!locked && v->refs==2);--v->refs;}
static int vm_map_object(vm_space_t *v,vm_object_t *o,vaddr_t *address,uint64_t off,size_t size,unsigned prot,unsigned flags){
 assert(o->refs==2 && !off && size==8192 && prot==7 && flags==8 && *address==0x10000);
 ++maps;inject(v,1);*address=0x20000;return map_status;
}
static int vm_populate_range(vm_space_t *v,vaddr_t address,size_t size){assert(address==0x20000 && size==8192);++populates;inject(v,2);return populate_status;}
static int vm_unmap(vm_space_t *v,vaddr_t address,size_t size){assert(v==expected && !locked && v->refs && address==0x20000 && size==8192);++unmaps;return -5;}
static uint64_t telemetry_timestamp(void){return 100;}
static uint64_t window_tsc_to_us(uint64_t t){return t;}
static void kernel_frame_trace_emit_scope(const char *s,uint64_t t){(void)s;(void)t;assert(!locked);}
static void kernel_frame_trace_emit_value(const char *s,uint64_t t){(void)s;(void)t;assert(!locked);}
@BODY@
int main(void){
 vm_object_t object={1};section_t section={&object};window_server_window_t window={&section,8192};
 const int statuses[]={0,-12,-5};unsigned cases=0;
 for(unsigned m=0;m<3;++m)for(unsigned s=1;s<=2;++s)
 for(unsigned a=0;a<3;++a)for(unsigned b=0;b<3;++b){
  p.vm=allocate();expected=p.vm;mode=m;stage=s;map_status=statuses[a];populate_status=statuses[b];
  maps=populates=unmaps=destroyed=0;uint64_t address=999;
  int want=map_status?map_status:populate_status;
  assert(window_server_map(&window,&p,0x10000,&address)==want);
  assert(address==(want?999:0x20000) && maps==1 && populates==!a && unmaps==(!a && b!=0) && object.refs==1);
  bool detached=m && (s==1 || !a);
  assert(destroyed==detached);
  if(p.vm){assert(p.vm->refs==1);vm_space_put(p.vm);}
  assert(destroyed==(detached && m==2?2:1));++cases;
 }
 p.vm=allocate();mode=0;destroyed=0;uint64_t address=999;
 assert(window_server_map(NULL,&p,0,&address)==K_EINVAL);
 assert(window_server_map(&window,NULL,0,&address)==K_EINVAL);
 assert(window_server_map(&window,&p,0,NULL)==K_EINVAL);
 section.vm_object=NULL;
 assert(window_server_map(&window,&p,0,&address)==K_EIO && p.vm->refs==1);
 vm_space_put(p.vm);p.vm=NULL;section.vm_object=&object;
 assert(window_server_map(&window,&p,0,&address)==K_EINVAL && address==999);
 assert(cases==54);return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    directory = Path(directory)
    def run(name, implementation, pin=helper, harness=fixture, trace=0):
        path = directory / (name + '.c')
        path.write_text(harness.replace('@HELPER@', pin).replace('@BODY@', implementation))
        binary = directory / name
        subprocess.run(['cc', '-std=c11', '-fsanitize=address,undefined', '-fno-pie',
                        '-no-pie', f'-DOS_ENABLE_FRAME_TRACE={trace}', str(path), '-o', str(binary)], check=True)
        return subprocess.run([str(binary)], capture_output=True, text=True)
    for trace in (0, 1):
        result = run('map'+str(trace), body, trace=trace)
        assert result.returncode == 0, result.stderr
    injected = fixture.replace('unsigned m=0;', 'unsigned m=1;')
    result = run('borrowed', body, helper.replace('if (space != 0) vm_space_get(space);', ''), injected)
    assert result.returncode != 0 and 'heap-use-after-free' in result.stderr, result.stderr
    result = run('wrong_vm', body.replace('vm_populate_range(space,', 'vm_populate_range(p.vm,'), harness=injected)
    assert result.returncode != 0, result.stderr
    result = run('wrong_rollback', body.replace('vm_unmap(space,', 'vm_unmap(p.vm,'), harness=injected)
    assert result.returncode != 0, result.stderr
    result = run('leaked_pin', body.replace('    vm_space_put(space);\n    return status;',
                                          '    return status;'))
    assert result.returncode != 0, result.stderr
print('window map snapshot: 54 map/populate/exit/exec/failure cases per trace mode and validation/ownership paths passed; missing pin, wrong populate/rollback VM and leaked pin negatives rejected')
