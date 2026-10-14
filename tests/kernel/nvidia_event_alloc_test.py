"""Compile the real RM allocation branch and verify 595.84 fd policy."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/drivers/gpu/nvidia/rm/nv_rm_vendor_bridge.c').read_text()
branch = source[source.index('    case NV_FRONTEND_ESC_RM_ALLOC: {'):
                source.index('    case NV_ESC_RM_VID_HEAP_CONTROL: {')]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define NV_FRONTEND_ESC_RM_ALLOC 43
#define K_EINVAL -22
#define K_OK 0
enum {NV01_EVENT=5, NV01_EVENT_OS_EVENT=0x79,
      NV01_EVENT_KERNEL_CALLBACK=0x78, NV01_EVENT_KERNEL_CALLBACK_EX=0x7e};
typedef struct {uint32_t root,parent,object,hClass;uint64_t params,rights;
 uint32_t size,flags,status,padding;} NVOS64_PARAMETERS;
typedef int API_SECURITY_INFO;
static unsigned calls,security_calls;
static void nv_rm_vendor_frontend_security(void *f,API_SECURITY_INFO *s){
 assert(f);++security_calls;*s=42;
}
static void Nv04AllocWithAccessSecInfo(NVOS64_PARAMETERS *a,API_SECURITY_INFO s){
 assert(s==42);++calls;a->status=0x1234;
}
static int allocate(bool control,uint32_t cls,size_t size){
 struct {bool control;} raw={control},*file=&raw;
 NVOS64_PARAMETERS payload={.hClass=cls},*argument=&payload;
 size_t argument_size=size;
 switch(NV_FRONTEND_ESC_RM_ALLOC){
''' + branch + r'''
 default:return -999;
 }
}
int main(void){
 const uint32_t events[]={NV01_EVENT,NV01_EVENT_OS_EVENT,
  NV01_EVENT_KERNEL_CALLBACK,NV01_EVENT_KERNEL_CALLBACK_EX};
 for(unsigned i=0;i<4;++i)for(unsigned ctl=0;ctl<2;++ctl){
  unsigned before=calls;
  assert(allocate(ctl,events[i],48)==0 && calls==before+1);
 }
 for(unsigned cls=0;cls<512;++cls){
  bool event=false;for(unsigned i=0;i<4;++i)event|=events[i]==cls;
  unsigned before=calls;
  assert(allocate(false,cls,48)==(event?0:-22));
  assert(calls==before+(event?1:0));
  assert(allocate(true,cls,48)==0);
 }
 for(unsigned size=0;size<80;++size)if(size!=48){
  unsigned before=calls;
  assert(allocate(false,NV01_EVENT_OS_EVENT,size)==-22);
  assert(allocate(true,NV01_EVENT_OS_EVENT,size)==-22 && calls==before);
 }
 assert(calls==security_calls);
}
'''
with tempfile.TemporaryDirectory(prefix='nv-event-alloc-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(path / 'test.c'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('NVIDIA event alloc: actual GPU/control fd policy, 512 classes, malformed sizes and RM dispatch PASS')
