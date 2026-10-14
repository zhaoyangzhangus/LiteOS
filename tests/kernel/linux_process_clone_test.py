"""Execute real fork/clone helpers with child-COW pins and failure fixtures."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/syscall/process.c').read_text()
begin = source.index('static kstatus_t process_fork_child_tid_write(')
end = source.index('int64_t syscall_process_wait(', begin)
production = source[begin:end]
wait_end = source.index('int64_t syscall_thread_context(', end)
production += source[end:wait_end]
linux = (root/'kernel/syscall/linux.c').read_text()
clone = linux[linux.index('static int64_t linux_clone('):]
assert clone.index('syscall_process_clone_linux(') < clone.index('child_stack == 0U')
assert 'case LINUX_SYS_FORK:' in linux and 'case LINUX_SYS_WAIT4:' in linux
start = linux.index('    case LINUX_SYS_FORK:')
dispatch = linux[start:linux.index('    case LINUX_SYS_PRCTL:', start)]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define __user
#define PAGE_SIZE 16U
#define X86_64_USER_TOP 0x7fffffffffffULL
#define OS_SIGCHLD 17U
#define LINUX_CLONE_PARENT_SETTID 0x00100000ULL
#define LINUX_CLONE_CHILD_CLEARTID 0x00200000ULL
#define LINUX_CLONE_CHILD_SETTID 0x01000000ULL
enum {K_OK=0,K_EPERM=-1,K_EFAULT=-14,K_EINVAL=-22,K_ECANCELED=-125,K_ENOMEM=-12,K_ENOSYS=-38};
#define LINUX_SYS_FORK 57U
#define LINUX_SYS_WAIT4 61U
#define OS_PROCESS_WAIT_NOHANG 1U
typedef int64_t kstatus_t;typedef uint64_t vaddr_t;
typedef struct {uint64_t rax,rsp,vector,error_code,rdi,rsi,rdx,r10;} arch_trap_frame_t;
typedef struct {int identity;} vm_space_t;
typedef struct {uint8_t data[16];} page_t;
typedef struct {uint64_t fs_base;} arch_t;
typedef struct {uint64_t pid;vm_space_t* vm;} process_t;
typedef struct {uint64_t tid;arch_t arch;uint64_t signal_mask,linux_clear_tid;bool linux_clear_tid_pending;} thread_t;
static vm_space_t parent_space={1},child_space={2};
static process_t parent_process={70,&parent_space},child_process={88,&child_space};
static thread_t parent_thread={.tid=71,.arch={0x7000},.signal_mask=7},child_thread;
static page_t pages[2];static uint8_t parent_tls[32];static uint32_t parent_tid_word;
static arch_trap_frame_t original={.rax=99,.rsp=0x3000,.vector=128,.error_code=1};
static int failure,forks,creates,starts,pins,unpins,terminates,aborts,process_puts,thread_puts,fp_clones;
static arch_trap_frame_t created_frame;
static int vm_refs,vm_gets,vm_puts,locks;
static int wait_calls;static int64_t waited_pid=88,waited_exit=7;static uint32_t waited_signal;
static process_t* current_process(void){return &parent_process;}
static void process_thread_lock(process_t* process){assert(process==&child_process&&!locks);++locks;}
static void process_thread_unlock(process_t* process){assert(process==&child_process&&locks==1);--locks;}
static void vm_space_get(vm_space_t* space){assert(space==&child_space&&locks==1);++vm_refs;++vm_gets;}
static void vm_space_put(vm_space_t* space){assert(space==&child_space&&vm_refs==1&&!locks);--vm_refs;++vm_puts;}
static thread_t* sched_current_thread(void){return &parent_thread;}
static bool x86_validate_user_frame(arch_trap_frame_t* frame){return frame&&failure!=1;}
static bool x86_user_range_valid(const void* pointer,size_t size){
 uintptr_t p=(uintptr_t)pointer;
 return (p>=0x2000&&p<=0x2020-size)||p==(uintptr_t)&parent_tid_word;
}
static kstatus_t process_fork(process_t* parent,process_t** out){
 assert(parent==&parent_process);++forks;if(failure==2)return K_ENOMEM;
 memcpy(pages,parent_tls,32);*out=&child_process;return K_OK;
}
static kstatus_t thread_create_user_from_frame(process_t* child,arch_trap_frame_t* frame,vaddr_t fs,thread_t** out){
 assert(child==&child_process&&fs==0x7000);++creates;created_frame=*frame;
 if(failure==3)return K_ENOMEM;
 memset(&child_thread,0,sizeof(child_thread));child_thread.tid=99;*out=&child_thread;return K_OK;
}
static void x86_fp_state_clone_current(arch_t* parent,arch_t* child){assert(parent==&parent_thread.arch&&child==&child_thread.arch);++fp_clones;}
static kstatus_t vm_pin_user_page(vm_space_t* space,vaddr_t address,bool writable,page_t** out){
 assert(space==&child_space&&vm_refs==1&&!locks&&writable&&(address==0x2000||address==0x2010));++pins;
 if(failure==4||(failure==5&&address==0x2010))return K_EFAULT;
 *out=&pages[(address-0x2000)/16];return K_OK;
}
static uintptr_t page_to_phys(page_t* page){return (uintptr_t)page->data;}
static void* phys_to_direct(uintptr_t physical){return failure==6?0:(void*)physical;}
static void vm_unpin_user_page(page_t* page){assert(page==&pages[0]||page==&pages[1]);++unpins;}
static kstatus_t put_user_u32(uint32_t* address,uint32_t value){assert(address==&parent_tid_word&&value==88);if(failure==7)return K_EFAULT;*address=value;return K_OK;}
static kstatus_t thread_start(thread_t* thread){assert(thread==&child_thread&&thread->signal_mask==7);++starts;return failure==8?K_ENOMEM:K_OK;}
static kstatus_t thread_terminate(thread_t* thread,int64_t status){assert(thread==&child_thread&&status==K_ECANCELED);++terminates;return K_OK;}
static void process_abort(process_t* process){assert(process==&child_process);++aborts;}
static void object_put(void* object){if(object==&child_process)++process_puts;else{assert(object==&child_thread);++thread_puts;}}
static kstatus_t process_wait_child(process_t* parent,int64_t pid,uint32_t options,int64_t* child,int64_t* code,uint32_t* signal){
 assert(parent==&parent_process&&(pid==88||pid==-1));assert(options<=1);++wait_calls;
 *child=waited_pid;*code=waited_exit;*signal=waited_signal;return K_OK;
}
static kstatus_t copy_to_user(void* destination,const void* source,size_t size){assert(size==4);if(failure==9)return K_EFAULT;memcpy(destination,source,size);return K_OK;}
''' + production + r'''
static int64_t LinuxDispatch(uint64_t number,arch_trap_frame_t* frame){switch(number){
''' + dispatch + r'''
 default:return K_ENOSYS;
}}
static void reset(int mode){
 failure=mode;forks=creates=starts=pins=unpins=terminates=aborts=process_puts=thread_puts=fp_clones=0;
 assert(!vm_refs&&!locks);vm_gets=vm_puts=0;
 memset(parent_tls,0x5a,sizeof(parent_tls));memset(pages,0,sizeof(pages));parent_tid_word=0;
}
int main(void){
 uint64_t frame=(uintptr_t)&original;
 uint64_t flags=OS_SIGCHLD|LINUX_CLONE_PARENT_SETTID|LINUX_CLONE_CHILD_SETTID|LINUX_CLONE_CHILD_CLEARTID;
 reset(0);assert(syscall_process_clone_linux(flags,0,(uintptr_t)&parent_tid_word,0x200e,frame)==88);
 uint32_t child_tid=0;memcpy(&child_tid,((uint8_t*)pages)+14,4);assert(child_tid==88&&parent_tid_word==88);
 for(unsigned i=0;i<32;++i)assert(parent_tls[i]==0x5a);
 assert(pins==2&&unpins==2&&starts==1&&process_puts==1&&thread_puts==1&&!aborts);
 assert(vm_gets==1&&vm_puts==1&&!vm_refs&&!locks);
 reset(0);child_process.vm=0;
 assert(syscall_process_clone_linux(flags,0,(uintptr_t)&parent_tid_word,0x200e,frame)==K_EINVAL);
 assert(!pins&&!vm_gets&&!vm_puts&&aborts==1&&terminates==1);
 child_process.vm=&child_space;
 reset(0);assert(syscall_process_clone_linux(flags,0,(uintptr_t)&parent_tid_word,0x200e,frame)==88);
 assert(child_thread.linux_clear_tid==0x200e&&child_thread.linux_clear_tid_pending);
 assert(created_frame.rax==0&&created_frame.rsp==original.rsp&&!created_frame.vector&&!created_frame.error_code&&fp_clones==1);
 assert(original.rax==99&&original.vector==128&&original.error_code==1);
 reset(0);assert(syscall_process_fork(1,2,3,4,5,frame)==88);assert(!pins&&!child_thread.linux_clear_tid_pending);
 reset(0);assert(syscall_process_clone_linux(OS_SIGCHLD,0x4000,0,0,frame)==88);assert(created_frame.rsp==0x4000);
 for(unsigned bit=8;bit<64;++bit){uint64_t option=1ULL<<bit;if(option&flags)continue;
  reset(0);assert(syscall_process_clone_linux(OS_SIGCHLD|option,0,0,0,frame)==K_EINVAL);assert(!forks);
 }
 for(uint64_t stack=1;stack<16;++stack){reset(0);assert(syscall_process_clone_linux(OS_SIGCHLD,stack,0,0,frame)==K_EINVAL);assert(!forks);}
 reset(0);assert(syscall_process_clone_linux(flags,0,1,0x2000,frame)==K_EFAULT&&!forks);
 reset(0);assert(syscall_process_clone_linux(flags,0,(uintptr_t)&parent_tid_word,0x201f,frame)==K_EFAULT&&!forks);
 reset(0);assert(syscall_process_clone_linux(0,0,0,0,frame)==K_EINVAL&&!forks);
 for(int mode=1;mode<=8;++mode){reset(mode);assert(syscall_process_clone_linux(flags,0,(uintptr_t)&parent_tid_word,0x200e,frame)<0);
  if(mode==1)assert(!forks);
  if(mode==2)assert(!aborts&&!process_puts);
  if(mode>=3)assert(aborts==1&&process_puts==1);
  if(mode>=4)assert(terminates==1&&thread_puts==1);
  if(mode==4)assert(!unpins&&!starts);
  if(mode==5)assert(pins==2&&unpins==1&&!starts);
  if(mode==6)assert(pins==1&&unpins==1&&!starts);
  if(mode==7)assert(unpins==2&&!starts);
  if(mode==8)assert(starts==1);
  for(unsigned i=0;i<32;++i)assert(parent_tls[i]==0x5a);
  assert(vm_gets==vm_puts&&!vm_refs&&!locks);
 }
 reset(0);assert(LinuxDispatch(LINUX_SYS_FORK,&original)==88);
 int32_t wait_status=-1;arch_trap_frame_t request={.rdi=88,.rsi=(uintptr_t)&wait_status};
 assert(LinuxDispatch(LINUX_SYS_WAIT4,&request)==88&&wait_status==(7<<8)&&wait_calls==1);
 request.rdi=(uint64_t)-1;waited_signal=9;wait_status=-1;
 assert(LinuxDispatch(LINUX_SYS_WAIT4,&request)==88&&wait_status==9);
 waited_signal=0;waited_pid=0;request.rdx=OS_PROCESS_WAIT_NOHANG;wait_status=123;
 assert(LinuxDispatch(LINUX_SYS_WAIT4,&request)==0&&wait_status==123);
 int calls=wait_calls;request.rdx=2;
 assert(LinuxDispatch(LINUX_SYS_WAIT4,&request)==K_EINVAL&&wait_calls==calls);
 request.rdx=0;request.r10=1;
 assert(LinuxDispatch(LINUX_SYS_WAIT4,&request)==K_ENOSYS&&wait_calls==calls);
 request.r10=0;request.rdi=0;
 assert(LinuxDispatch(LINUX_SYS_WAIT4,&request)==K_EINVAL&&wait_calls==calls);
 request.rdi=88;waited_pid=88;failure=9;
 assert(LinuxDispatch(LINUX_SYS_WAIT4,&request)==K_EFAULT);
}
'''
with tempfile.TemporaryDirectory(prefix='linux-process-clone-') as directory:
    base = Path(directory)
    (base/'test.c').write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(base/'test.c'), '-o', str(base/'test')], check=True)
    subprocess.run([str(base/'test')], check=True)
print('Linux process clone: actual native/fork helper, COW child TID including '
      'page crossing, PID/TID identity, TLS/FP/signal inheritance, flag validation '
      'and allocation/pin/store/start rollback passed')
