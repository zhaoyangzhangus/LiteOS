"""Actual VM syscall wrappers/map transaction: retained operation and rollback."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/syscall/vm.c').read_text()
internal = (root / 'kernel/process/internal.h').read_text()
helper = internal[internal.index('static inline vm_space_t *process_vm_reference('):]
translate = source[source.index('uint32_t translate_vm_protection('):source.index('/* VM_MAP(args)')]
start = source.index('map_object:')
mapping = source[start:source.index('/* VM_SHARE', start)].replace('map_object:', '')
wrappers = source[source.index('int64_t syscall_vm_unmap('):]
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <uapi/mm.h>
#define __user
#define K_OK 0
#define K_EINVAL -22
#define K_EPERM -1
#define VM_PROT_USER 8U
#define VM_PROT_READ 1U
#define VM_PROT_WRITE 2U
#define VM_PROT_EXEC 4U
#define VM_MAP_PRIVATE 1U
#define VM_MAP_SHARED 2U
#define VM_MAP_FIXED 4U
#define VM_MAP_STACK 8U
typedef int kstatus_t;
typedef uint64_t vaddr_t;
typedef struct {unsigned refs;} vm_space_t;
typedef struct {vm_space_t *vm;} process_t;
typedef struct {unsigned refs;} vm_object_t;
static process_t process;
static bool held,absent,detach,swap_on_copy,copy_fail;
static unsigned destroyed,operations,object_puts;
static int backend_status;
static vm_space_t *expected;
static vm_space_t *allocate(void) {vm_space_t *p=malloc(sizeof(*p));assert(p);p->refs=1;return p;}
static process_t *current_process(void) {return absent?NULL:&process;}
static void process_thread_lock(process_t *p) {assert(p==&process && !held);held=true;}
static void process_thread_unlock(process_t *p) {assert(p==&process && held);held=false;}
static void vm_space_get(vm_space_t *s) {assert(held && s->refs);++s->refs;}
static void vm_space_put(vm_space_t *s) {assert(!held);if(s && !--s->refs){++destroyed;free(s);}}
static int operation(vm_space_t *s) {
 assert(!held);++operations;if(!s)return K_EINVAL;
 assert(s==expected && s->refs);
 if(detach){detach=false;process_thread_lock(&process);process.vm=NULL;process_thread_unlock(&process);vm_space_put(s);}
 assert(s->refs);return backend_status;
}
static int vm_unmap(vm_space_t *s,vaddr_t a,size_t n) {assert(a==0x10000 && n==4096);return operation(s);}
static int vm_protect(vm_space_t *s,vaddr_t a,size_t n,uint32_t prot) {assert(prot==(VM_PROT_USER|VM_PROT_READ));return vm_unmap(s,a,n);}
static int vm_sync(vm_space_t *s,vaddr_t a,size_t n,uint32_t f) {assert(f==1);return vm_unmap(s,a,n);}
static int vm_advise(vm_space_t *s,vaddr_t a,size_t n,uint32_t f) {assert(f==1);return vm_unmap(s,a,n);}
static int vm_map_object(vm_space_t *s,vm_object_t *o,vaddr_t *a,uint64_t off,size_t n,uint32_t prot,uint32_t flags) {
 assert(o && off==0 && n==4096 && prot==(VM_PROT_USER|VM_PROT_READ) && flags==VM_MAP_PRIVATE);*a=0x10000;return operation(s);
}
static void vm_object_put(vm_object_t *o) {assert(!held && o->refs==1);--o->refs;++object_puts;}
static int copy_to_user(void *out,const void *in,size_t n) {
 assert(!held && expected->refs);
 if(swap_on_copy) {
  process_thread_lock(&process);vm_space_t *old=process.vm;process.vm=allocate();process_thread_unlock(&process);vm_space_put(old);
 }
 if(copy_fail)return -14;
 *(os_vm_map_args_t *)out=*(const os_vm_map_args_t *)in;assert(n==sizeof(os_vm_map_args_t));return K_OK;
}
static void kernel_serial_write(const char *s) {(void)s;}
static void kernel_serial_write_u32(uint32_t n) {(void)n;}
@HELPER@
@TRANSLATE@
static int map_transaction(os_vm_map_args_t arguments,vm_object_t *mapped_object,os_vm_map_args_t *output) {
 process_t *process=current_process();
 uint64_t arguments_pointer=(uintptr_t)output,mapped_offset=0;int status;
 @MAPPING@
@WRAPPERS@
static void setup(void) {
 held=absent=detach=swap_on_copy=copy_fail=false;destroyed=operations=object_puts=0;backend_status=0;
 process.vm=expected=allocate();
}
static void finish(void) {if(process.vm)vm_space_put(process.vm);process.vm=NULL;assert(destroyed==1 && !held);}
int main(void) {
 int64_t (*calls[])(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t)={syscall_vm_unmap,syscall_vm_protect,syscall_vm_sync,syscall_vm_advise};
 for(unsigned i=0;i<4;++i)for(unsigned mode=0;mode<4;++mode) {
  setup();detach=mode&1;backend_status=(mode&2)?-12:0;
  assert(calls[i](0x10000,4096,1,0,0,0)==backend_status && operations==1);finish();
 }
 for(unsigned i=0;i<4;++i) {
  setup();absent=true;assert(calls[i](0x10000,4096,1,0,0,0)==K_EPERM && !operations);finish();
  setup();vm_space_put(process.vm);process.vm=NULL;
  assert(calls[i](0x10000,4096,1,0,0,0)==K_EINVAL && operations==1);finish();
 }
 for(unsigned mode=0;mode<8;++mode) {
  setup();detach=mode&1;copy_fail=mode&2;backend_status=(mode&4)?-12:0;
  os_vm_map_args_t args={.length=4096,.prot=OS_VM_READ,.flags=OS_VM_PRIVATE},out={0};vm_object_t object={.refs=1};
  assert(map_transaction(args,&object,&out)==(backend_status?backend_status:copy_fail?-14:0));
  assert(object_puts==1 && !object.refs && operations==(backend_status || !copy_fail?1:2));finish();
 }
 setup();swap_on_copy=copy_fail=true;os_vm_map_args_t args={.length=4096,.prot=OS_VM_READ,.flags=OS_VM_PRIVATE},out={0};vm_object_t object={.refs=1};
 assert(map_transaction(args,&object,&out)==-14 && operations==2 && destroyed==1);
 vm_space_put(process.vm);process.vm=NULL;assert(destroyed==2);
 return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    directory = Path(directory)
    def run(name, pin, transaction):
        path = directory / (name + '.c')
        path.write_text(fixture.replace('@HELPER@', pin).replace('@TRANSLATE@', translate)
                        .replace('@MAPPING@', transaction).replace('@WRAPPERS@', wrappers))
        binary = directory / name
        subprocess.run(['cc', '-std=c11', '-fsanitize=address,undefined', '-fno-pie',
                        '-no-pie', '-I'+str(root/'include'), str(path), '-o', str(binary)], check=True)
        return subprocess.run([str(binary)], capture_output=True, text=True)
    result = run('retained', helper, mapping)
    assert result.returncode == 0, result.stderr
    result = run('borrowed', helper.replace('if (space != 0) vm_space_get(space);', ''), mapping)
    assert result.returncode != 0 and 'heap-use-after-free' in result.stderr, result.stderr
    result = run('wrong_rollback', helper, mapping.replace('vm_unmap(space,', 'vm_unmap(process->vm,'))
    assert result.returncode != 0 and 's==expected' in result.stderr, result.stderr
print('VM syscall snapshot: unmap/protect/sync/advise and map rollback success/error/exit/NULL/replaced-VM cases passed; borrowed VM and wrong rollback negatives rejected')
