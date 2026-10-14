"""Actual GPU map handler: one retained VM through page faults and rollback."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/syscall/graphics.c').read_text()
body = source[source.index('int64_t syscall_gpu_map('):
              source.index('/* DISPLAY_COMMIT')]
internal = (root / 'kernel/process/internal.h').read_text()
helper = internal[internal.index('static inline vm_space_t *process_vm_reference('):]
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <uapi/gpu.h>
#include <uapi/mm.h>
#include <uapi/syscall.h>
#define __user
#define PAGE_SIZE 4096ULL
#define K_OK 0
#define K_EINVAL -22
#define K_EPERM -1
#define VM_MAP_SHARED 2
#define VM_MAP_FIXED 4
#define VM_PROT_USER 8
#define X86_CACHE_WB 0
#define GPU_ALLOCATION_RIGHT_READ 1
#define GPU_ALLOCATION_RIGHT_WRITE 2
#define KOBJECT_TYPE_GPU_ALLOCATION 5
typedef int kstatus_t;
typedef uint64_t vaddr_t;
typedef uint64_t paddr_t;
typedef unsigned handle_t;
typedef struct {unsigned refs;struct {unsigned type;} object;} gpu_allocation_t;
typedef struct {unsigned refs;gpu_allocation_t *allocation;} vm_object_t;
typedef struct {unsigned refs;vm_object_t *mapped;} vm_space_t;
typedef struct {vm_space_t *vm;unsigned handles;} process_t;
typedef struct {vaddr_t address;unsigned access,cpu_error;} vm_fault_info_t;
static process_t p;
static gpu_allocation_t allocation;
static vm_space_t *expected;
static os_gpu_map_t args;
static bool locked,map_fail,copy_fail,absent;
static unsigned n,mode,stage,fault_failure,faults,maps,unmaps,copies,vm_destroyed,object_destroyed,gets,rights;
static void object_get(void *raw){gpu_allocation_t *a=raw;assert(!locked && a==&allocation && a->refs);++a->refs;}
static void object_put(void *raw){gpu_allocation_t *a=raw;assert(!locked && a==&allocation && a->refs>1);--a->refs;}
static void vm_object_put(vm_object_t *v){assert(!locked && v->refs);if(!--v->refs){object_put(v->allocation);++object_destroyed;free(v);}}
static vm_space_t *allocate(void){vm_space_t *v=calloc(1,sizeof(*v));assert(v);v->refs=1;return v;}
static void vm_space_put(vm_space_t *v){assert(!locked);if(v && !--v->refs){if(v->mapped)vm_object_put(v->mapped);++vm_destroyed;free(v);}}
static void process_thread_lock(process_t *v){assert(v==&p && !locked);locked=true;}
static void process_thread_unlock(process_t *v){assert(v==&p && locked);locked=false;}
static void vm_space_get(vm_space_t *v){assert(locked && v->refs);++v->refs;++gets;}
@HELPER@
static process_t *current_process(void){return absent?NULL:&p;}
static bool versioned_header_valid(os_versioned_header_t *hdr,size_t size){return hdr->size==size && hdr->version==OS_SYSCALL_ABI_VERSION;}
static unsigned translate_vm_protection(unsigned prot){return prot;}
static int copy_from_user(void *to,const void *from,size_t size){assert(from==&args && size==sizeof(args));memcpy(to,from,size);return 0;}
static void inject(unsigned where){
 if(!mode || stage!=where)return;
 process_thread_lock(&p);assert(p.vm==expected);p.vm=mode==1?NULL:allocate();process_thread_unlock(&p);
 vm_space_put(expected);
}
static int copy_to_user(void *to,const void *from,size_t size){assert(to==&args && size==sizeof(args));++copies;inject(n+1);if(copy_fail)return -14;memcpy(to,from,size);return 0;}
static int handle_lookup(unsigned *table,handle_t handle,unsigned required,void **out){assert(table==&p.handles && handle==7);rights=required;object_get(&allocation);*out=&allocation;return 0;}
static int gpu_allocation_get_physical(gpu_allocation_t *a,uint64_t offset,uint64_t length,paddr_t *out){assert(a==&allocation && offset==8192 && length==n*PAGE_SIZE);*out=0x800000;return 0;}
static int vm_object_create_device(paddr_t physical,uint64_t length,unsigned cache,void *owner,void (*release)(void *),vm_object_t **out){
 assert(physical==0x800000 && length==n*PAGE_SIZE && cache==0 && owner==&allocation && release==object_put && allocation.refs==3);
 vm_object_t *v=calloc(1,sizeof(*v));assert(v);v->refs=1;v->allocation=owner;*out=v;return 0;
}
static int vm_map_object(vm_space_t *v,vm_object_t *o,vaddr_t *address,uint64_t offset,size_t length,unsigned prot,unsigned flags){
 assert(!locked && v==expected && v->refs==2 && o->refs==1 && !offset && length==n*PAGE_SIZE);
 assert(prot==(8U|args.prot) && flags==(2U|((args.flags&OS_GPU_MAP_FIXED)?4U:0U)) && *address==0x10000);
 ++maps;inject(0);assert(v->refs);if(map_fail)return -12;
 *address=0x20000;assert(!v->mapped);++o->refs;v->mapped=o;return 0;
}
static int vm_handle_fault(vm_space_t *v,vm_fault_info_t *fault){
 assert(!locked && v==expected && v->refs && v->mapped && fault->address==0x20000+faults*PAGE_SIZE && fault->access==args.prot && !fault->cpu_error);
 ++faults;inject(faults);assert(v->refs);return faults==fault_failure?-5:0;
}
static int vm_unmap(vm_space_t *v,vaddr_t address,size_t size){assert(!locked && v==expected && v->refs && v->mapped && address==0x20000 && size==n*PAGE_SIZE);++unmaps;vm_object_put(v->mapped);v->mapped=NULL;return 0;}
@BODY@
static void setup(void){
 p.vm=allocate();expected=p.vm;allocation=(gpu_allocation_t){.refs=1,.object={5}};
 maps=unmaps=faults=copies=vm_destroyed=object_destroyed=gets=rights=0;locked=absent=false;
 args=(os_gpu_map_t){.hdr={.size=sizeof(args),.version=OS_SYSCALL_ABI_VERSION},.allocation=7,.address=0x10000,.offset=8192,.length=n*PAGE_SIZE};
}
int main(void){
 unsigned cases=0;const unsigned sizes[]={1,2,8,64};
 for(unsigned size=0;size<4;++size)for(unsigned prot=1;prot<=3;++prot)
 for(unsigned fixed=0;fixed<2;++fixed)for(unsigned mf=0;mf<2;++mf)
 for(unsigned ff=0;ff<=sizes[size];++ff)for(unsigned cf=0;cf<2;++cf)
 for(unsigned m=0;m<3;++m)for(unsigned si=0;si<4;++si){
  n=sizes[size];const unsigned stages[]={0,1,n,n+1};mode=m;stage=stages[si];fault_failure=ff;map_fail=mf;copy_fail=cf;setup();args.prot=prot;args.flags=fixed?OS_GPU_MAP_FIXED:0;
  int want=mf?-12:ff?-5:cf?-14:0;
  assert(syscall_gpu_map((uint64_t)(uintptr_t)&args,0,0,0,0,0)==want);
  assert(maps==1 && gets==1 && rights==prot && faults==(mf?0:ff?ff:n) && copies==(!mf && !ff));
  assert(unmaps==(!mf && (ff || cf)) && args.address==(want?0x10000:0x20000));
  bool detached=m && (stage==0 || (!mf && stage<=(ff?ff:n+1)));
  assert(vm_destroyed==detached && !locked);
  if(p.vm){assert(p.vm->refs==1);vm_space_put(p.vm);p.vm=NULL;}
  assert(vm_destroyed==(detached && m==2?2:1) && object_destroyed==1 && allocation.refs==1);++cases;
 }
 assert(cases==22752);
 n=1;setup();args.prot=0;
 assert(syscall_gpu_map((uint64_t)(uintptr_t)&args,0,0,0,0,0)==K_EINVAL && !gets && allocation.refs==1);
 args.prot=1;args.length=1;
 assert(syscall_gpu_map((uint64_t)(uintptr_t)&args,0,0,0,0,0)==K_EINVAL && !gets && allocation.refs==1);
 absent=true;assert(syscall_gpu_map(0,0,0,0,0,0)==K_EPERM);
 vm_space_put(p.vm);return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    directory = Path(directory)
    def run(name, implementation=body, pin=helper, harness=fixture):
        path = directory / (name + '.c')
        path.write_text(harness.replace('@HELPER@', pin).replace('@BODY@', implementation))
        binary = directory / name
        subprocess.run(['cc', '-std=c11', '-fsanitize=address,undefined', '-fno-pie',
                        '-no-pie', '-I', str(root/'include'), str(path), '-o', str(binary)], check=True)
        return subprocess.run([str(binary)], capture_output=True, text=True)
    result = run('gpu')
    assert result.returncode == 0, result.stderr
    injected = fixture.replace('unsigned m=0;', 'unsigned m=1;').replace('v->refs==2', 'v->refs>=1')
    result = run('borrowed', pin=helper.replace('if (space != 0) vm_space_get(space);', ''), harness=injected)
    assert result.returncode != 0 and 'heap-use-after-free' in result.stderr, result.stderr
    result = run('wrong_fault', body.replace('vm_handle_fault(space,', 'vm_handle_fault(process->vm,'))
    assert result.returncode != 0, result.stderr
    result = run('wrong_rollback', body.replace('vm_unmap(space,', 'vm_unmap(process->vm,'))
    assert result.returncode != 0, result.stderr
    result = run('leaked_pin', body.replace('    vm_space_put(space);', ''))
    assert result.returncode != 0, result.stderr
print('GPU map snapshot: 22752 page/protection/fixed/map/fault/copy/exit/exec cases with exact rollback and allocation lifetime passed; missing pin/UAF, wrong fault/rollback VM and leaked pin negatives rejected')
