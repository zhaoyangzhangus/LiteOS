"""Production readv/writev: aggregate bounds before I/O and partial progress."""
from pathlib import Path
import resource
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/syscall/linux.c').read_text()
body=source[source.index('static int64_t linux_readv('):source.index('static int64_t linux_nanosleep(')]
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#define __user
#define LINUX_MAX_IOV 64U
#define K_OK 0
#define K_EOVERFLOW -75
#define LINUX_EFAULT -14
typedef struct {uint64_t base,length;} linux_iovec_t;
typedef int process_t;
static unsigned io_calls,copy_calls,fail_at,short_at;
static bool fail_copy,writing_expected;
static int64_t lengths[64];
static int copy_from_user(void *out,const void *in,size_t size) {
 ++copy_calls;if(fail_copy)return LINUX_EFAULT;
 memcpy(out,in,size);return K_OK;
}
static int64_t transfer(process_t *p,int64_t descriptor,uint64_t base,uint64_t length,bool writing) {
 assert(!p && descriptor==19 && writing==writing_expected);
 unsigned index=io_calls++;
 assert(base==index+17 && length==(uint64_t)lengths[index]);
 if(index==fail_at)return -5;
 if(index==short_at)return (int64_t)(length/2);
 return (int64_t)length;
}
static int64_t linux_read(process_t *p,int64_t fd,uint64_t base,uint64_t length) {
 return transfer(p,fd,base,length,false);
}
static int64_t linux_write(process_t *p,int64_t fd,uint64_t base,uint64_t length) {
 return transfer(p,fd,base,length,true);
}
''' + body + r'''
static linux_iovec_t vectors[64];
static int64_t oracle(unsigned count) {
 int64_t total=0;
 for(unsigned i=0;i<count;++i) {
  if(i==fail_at)return total?total:-5;
  if(i==short_at) {total+=lengths[i]/2;if(lengths[i])break;}
  else total+=lengths[i];
 }
 return total;
}
int main(void) {
 unsigned cases=0;
 for(unsigned writing=0;writing<2;++writing)for(unsigned count=0;count<=64;++count)
 for(unsigned mode=0;mode<3;++mode)for(unsigned stop=0;stop<=count;++stop)
 for(unsigned short_case=0;short_case<2;++short_case) {
  writing_expected=writing;fail_at=short_case?UINT32_MAX:stop;short_at=short_case?stop:UINT32_MAX;
  for(unsigned i=0;i<count;++i) {
   lengths[i]=mode==0?0:mode==1?1:(i*17+count)%4097;
   vectors[i]=(linux_iovec_t){i+17,(uint64_t)lengths[i]};
  }
  copy_calls=io_calls=0;
  assert(linux_readv(0,19,(uintptr_t)vectors,count,writing)==oracle(count));
  assert(copy_calls==(count?1U:0U));
  unsigned expected_calls=count;
  if(stop<count && (!short_case || lengths[stop]))expected_calls=stop+1;
  assert(io_calls==expected_calls);++cases;
 }
 for(unsigned writing=0;writing<2;++writing)for(unsigned count=1;count<=64;++count)
 for(unsigned bad=0;bad<count;++bad) {
  writing_expected=writing;
  for(unsigned i=0;i<count;++i){vectors[i]=(linux_iovec_t){i+17,1};lengths[i]=1;}
  vectors[bad].length=(uint64_t)INT64_MAX+1;
  copy_calls=io_calls=0;
  assert(linux_readv(0,19,(uintptr_t)vectors,count,writing)==K_EOVERFLOW);
  assert(!io_calls && copy_calls==1);
  if(count>1) {
   for(unsigned i=0;i<count;++i){vectors[i].length=0;lengths[i]=0;}
   vectors[bad].length=INT64_MAX;vectors[(bad+1)%count].length=1;
   lengths[bad]=INT64_MAX;lengths[(bad+1)%count]=1;
   copy_calls=io_calls=0;
   assert(linux_readv(0,19,(uintptr_t)vectors,count,writing)==K_EOVERFLOW && !io_calls);
  }
 }
 writing_expected=true;lengths[0]=INT64_MAX;vectors[0]=(linux_iovec_t){17,INT64_MAX};
 fail_at=short_at=UINT32_MAX;copy_calls=io_calls=0;
 assert(linux_readv(0,19,(uintptr_t)vectors,1,true)==INT64_MAX && io_calls==1);
 lengths[0]=INT64_MAX-1;lengths[1]=1;
 vectors[0].length=INT64_MAX-1;vectors[1]=(linux_iovec_t){18,1};io_calls=0;
 assert(linux_readv(0,19,(uintptr_t)vectors,2,true)==INT64_MAX && io_calls==2);
 fail_copy=true;io_calls=0;
 assert(linux_readv(0,19,(uintptr_t)vectors,2,true)==LINUX_EFAULT && !io_calls);
 fail_copy=false;copy_calls=0;
 assert(linux_readv(0,19,0,65,true)==LINUX_EFAULT && !copy_calls && !io_calls);
 assert(linux_readv(0,19,0,0,true)==0 && !copy_calls && !io_calls);
 printf("Vector I/O: %u progress cases, 4160 invalid-position cases, aggregate overflow before side effects, INT64_MAX boundaries, copy faults and short/error returns passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-readv-progress-') as directory:
    binary=str(Path(directory)/'test')
    command=['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
             '-fsanitize=address,undefined','-fno-pie','-no-pie','-x','c','-','-o',binary]
    subprocess.run(command,input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
    start=code.index('    /* Reject an unrepresentable aggregate')
    end=code.index('    int64_t total = 0;',start)
    negative=code[:start]+code[end:]
    old_loop='    for (uint64_t index = 0U; index < count; ++index) {\n'
    assert negative.count(old_loop)==1
    negative=negative.replace(old_loop,old_loop+
        '        if (vectors[index].length > (uint64_t)INT64_MAX) return K_EOVERFLOW;\n')
    subprocess.run(command,input=negative,text=True,check=True)
    result=subprocess.run([binary],capture_output=True,text=True,timeout=30,
                          preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
    assert result.returncode and ('Assertion' in result.stderr or 'runtime error' in result.stderr)
    print('Negative control rejected: missing aggregate validation before vector I/O')
