"""Run the actual brk implementation with exit/exec/publication injection."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/syscall/linux.c').read_text()
body = source[source.index('static int64_t linux_brk('):
              source.index('static kstatus_t linux_timespec_timeout(')]
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdatomic.h>
#define PAGE_SIZE 4096ULL
#define K_EPERM -1
#define K_OK 0
#define PROCESS_RUNNING 1
#define VM_PROT_USER 1
#define VM_PROT_READ 2
#define VM_PROT_WRITE 4
#define VM_MAP_PRIVATE 1
#define VM_MAP_FIXED 2
typedef uint64_t vaddr_t;
typedef int kstatus_t;
typedef struct {unsigned refs;} vm_space_t;
typedef struct {vm_space_t *vm;uint64_t linux_brk_base,linux_brk;atomic_int state;} process_t;
static process_t p;
static vm_space_t *expected;
static bool locked;
static unsigned mode,maps,unmaps,destroyed,gets;
static int map_status;
static uint64_t start,amount;
static void process_thread_lock(process_t *v){assert(v==&p && !locked);locked=true;}
static void process_thread_unlock(process_t *v){assert(v==&p && locked);locked=false;}
static vm_space_t *allocate(void){vm_space_t *v=calloc(1,sizeof(*v));assert(v);v->refs=1;return v;}
static void vm_space_get(vm_space_t *v){assert(locked && v->refs);++v->refs;++gets;}
static void vm_space_put(vm_space_t *v){assert(!locked);if(v && !--v->refs){++destroyed;free(v);}}
static int vm_map_object(vm_space_t *v,void *obj,vaddr_t *addr,uint64_t off,size_t size,unsigned prot,unsigned flags){
 assert(!locked && v==expected && !obj && !off && *addr==start && size==amount);
 assert(prot==7 && flags==3);++maps;
 if(mode){
  process_thread_lock(&p);
  if(mode==1){p.vm=NULL;atomic_store(&p.state,3);}
  if(mode==2){p.vm=allocate();p.linux_brk_base=0x900000;p.linux_brk=0x901000;}
  if(mode==3)p.linux_brk+=1;
  if(mode==4)atomic_store(&p.state,2);
  process_thread_unlock(&p);
  if(mode==1 || mode==2)vm_space_put(v);
 }
 assert(v->refs);return map_status;
}
static int vm_unmap(vm_space_t *v,vaddr_t addr,size_t size){assert(!locked && v==expected && v->refs && addr==start && size==amount);++unmaps;return 0;}
@BODY@
static void setup(unsigned injection,int failure,uint64_t old){
 p.vm=allocate();p.linux_brk_base=0x10000;p.linux_brk=old;atomic_store(&p.state,1);
 expected=p.vm;mode=injection;map_status=failure;maps=unmaps=destroyed=gets=0;locked=false;
 start=(old+4095)&~4095ULL;
}
int main(void){
 assert(linux_brk(NULL,0)==K_EPERM);
 unsigned cases=0;
 const int failures[]={0,-12,-16,-5};
 for(unsigned offset=0;offset<8192;offset+=127)
 for(unsigned injection=0;injection<5;++injection)
 for(unsigned failure=0;failure<4;++failure){
  uint64_t old=0x10000+offset,request=old+8193;
  setup(injection,failures[failure],old);amount=((request+4095)&~4095ULL)-start;
  bool commit=!injection && !failure;
  assert(linux_brk(&p,request)==(int64_t)(commit?request:old));
  assert(maps==1 && gets==1 && unmaps==(!failure && injection!=0) && !locked);
  if(injection==1)assert(!p.vm && destroyed==1);
  else {
   assert(p.vm->refs==1);
   uint64_t want=injection==2?0x901000:injection==3?old+1:commit?request:old;
   assert(p.linux_brk==want);
   vm_space_put(p.vm);p.vm=NULL;
   assert(destroyed==(injection==2?2:1));
  }
  ++cases;
 }
 const uint64_t requests[]={0,0xffff,0x10001,0x10002,0x10003,UINT64_MAX};
 for(unsigned i=0;i<6;++i){
  setup(0,0,0x10002);uint64_t r=requests[i];
  bool fast=r<0x10000 || r==0x10002 || r>UINT64_MAX-4095;
  assert(linux_brk(&p,r)==(int64_t)(fast?0x10002:r));
  assert(!maps && !unmaps && gets==!fast && p.vm->refs==1);
  vm_space_put(p.vm);
 }
 setup(0,0,0);p.linux_brk_base=0;
 assert(linux_brk(&p,0)==0x60000000 && p.linux_brk_base==0x60000000 && !gets);
 vm_space_put(p.vm);
 assert(cases==1300);return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    directory = Path(directory)
    def run(name, implementation, harness=fixture):
        path = directory / (name + '.c')
        path.write_text(harness.replace('@BODY@', implementation))
        binary = directory / name
        subprocess.run(['cc', '-std=c11', '-fsanitize=address,undefined',
                        '-fno-pie', '-no-pie', str(path), '-o', str(binary)], check=True)
        return subprocess.run([str(binary)], capture_output=True, text=True)
    result = run('brk', body)
    assert result.returncode == 0, result.stderr
    result = run('borrowed', body.replace('if (space != 0) vm_space_get(space);', ''),
                 fixture.replace('unsigned injection=0;', 'unsigned injection=1;'))
    assert result.returncode != 0 and 'heap-use-after-free' in result.stderr, result.stderr
    guard = '''process->vm == space && process->linux_brk_base == base &&
        process->linux_brk == old_brk &&
        atomic_load_explicit(&process->state, memory_order_acquire) == PROCESS_RUNNING'''
    assert guard in body
    result = run('stale_commit', body.replace(guard, 'true'))
    assert result.returncode != 0, result.stderr
print('brk snapshot: 1300 growth/status/exit/exec/metadata cases plus query and same-page boundaries passed; missing pin and stale publication negatives rejected')
