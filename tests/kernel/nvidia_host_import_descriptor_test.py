"""Execute actual user-pointer-to-pinned-descriptor adapter and rollback."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/drivers/gpu/nvidia/rm/nv_rm_vendor_bridge.c').read_text()
begin=source.index('static void nv_rm_vendor_frontend_import_host_memory(')
end=source.index('kstatus_t nv_rm_vendor_frontend_ioctl(',begin)
code=r'''
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#define PAGE_SIZE 4096ULL
#define NV_OK 0
#define NV_ERR_NOT_SUPPORTED 0x56
#define NV_ERR_INVALID_LIMIT 0x32
#define NV_ERR_INVALID_ADDRESS 0x1e
#define NVOS32_DESCRIPTOR_TYPE_VIRTUAL_ADDRESS 0
#define NVOS32_DESCRIPTOR_TYPE_OS_PAGE_ARRAY 1
#define NvP64_VALUE(p) ((void*)(uintptr_t)(p))
#define FLD_TEST_DRF(os,field,name,value,v) ((v)&1)
typedef uint64_t NvP64,NvU64,NvUPtr;
typedef uint32_t NvU32,NV_STATUS;
typedef struct {int unused;} nv_rm_vendor_frontend_file_t,API_SECURITY_INFO;
typedef struct {unsigned function,status;
 struct {struct {NvP64 descriptor;NvU64 limit;NvU32 descriptorType,attr2;} AllocOsDesc;} data;} NVOS32_PARAMETERS;
static unsigned pins,unpins,core_calls,mode;
static int array_token;
static uint64_t wanted_pages;
static unsigned wanted_flags;
static NV_STATUS os_lock_user_pages(void *address,NvU64 pages,void **array,NvU32 flags) {
 assert(address==(void*)0x08000000 && pages==wanted_pages && flags==wanted_flags);++pins;
 if(mode==1)return NV_ERR_NOT_SUPPORTED;
 *array=mode==3?NULL:&array_token;return NV_OK;
}
static NV_STATUS os_unlock_user_pages(NvU64 pages,void *array,NvU32 flags) {
 assert(pages==wanted_pages && flags==wanted_flags && array==&array_token);++unpins;return NV_OK;
}
static void nv_rm_vendor_frontend_security(nv_rm_vendor_frontend_file_t *f,API_SECURITY_INFO *s) {assert(f && s);}
static void Nv04VidHeapControlWithSecInfo(NVOS32_PARAMETERS *heap,API_SECURITY_INFO s) {
 (void)s;assert(heap->data.AllocOsDesc.descriptor==(NvP64)(uintptr_t)&array_token);
 assert(heap->data.AllocOsDesc.descriptorType==1);++core_calls;
 heap->status=mode==2?0x56:0;
}
''' + source[begin:end] + r'''
int main(void) {
 nv_rm_vendor_frontend_file_t f={0};NVOS32_PARAMETERS h={0};
 for(unsigned writable=0;writable<2;++writable)for(unsigned pages=1;pages<=2;++pages)
 for(mode=0;mode<4;++mode) {
  pins=unpins=core_calls=0;wanted_pages=pages;wanted_flags=writable;
  h.data.AllocOsDesc.descriptor=0x08000000;h.data.AllocOsDesc.descriptorType=0;
  h.data.AllocOsDesc.limit=pages==1?4095:4096;h.data.AllocOsDesc.attr2=writable;
  nv_rm_vendor_frontend_import_host_memory(&f,&h);
  assert(pins==1 && unpins==(mode==2) && core_calls==(mode==0 || mode==2));
  assert(h.data.AllocOsDesc.descriptor==0x08000000 && !h.data.AllocOsDesc.descriptorType);
  assert(h.status==(mode==3?NV_ERR_INVALID_ADDRESS:mode?NV_ERR_NOT_SUPPORTED:0));
 }
 pins=unpins=core_calls=0;
 h.data.AllocOsDesc.descriptor=0;nv_rm_vendor_frontend_import_host_memory(&f,&h);
 assert(h.status==NV_ERR_INVALID_ADDRESS && !pins);
 h.data.AllocOsDesc.descriptor=0x08000001;nv_rm_vendor_frontend_import_host_memory(&f,&h);
 assert(h.status==NV_ERR_NOT_SUPPORTED && !pins);
 h.data.AllocOsDesc.descriptor=0x08000000;h.data.AllocOsDesc.limit=UINT64_MAX;
 nv_rm_vendor_frontend_import_host_memory(&f,&h);assert(h.status==NV_ERR_INVALID_LIMIT && !pins);
 h.data.AllocOsDesc.limit=4095;h.data.AllocOsDesc.descriptorType=3;
 nv_rm_vendor_frontend_import_host_memory(&f,&h);assert(h.status==NV_ERR_NOT_SUPPORTED && !pins && !core_calls && !unpins);
}
'''
with tempfile.TemporaryDirectory(prefix='nvidia-host-descriptor-') as directory:
    base=Path(directory);(base/'test.c').write_text(code)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',str(base/'test.c'),'-o',str(base/'test')],check=True)
    subprocess.run([str(base/'test')],check=True)
print('NVIDIA host descriptor: actual pin-before-core conversion, read/write flags, rounding, ownership transfer, core/pin/null failures, pointer restoration and invalid descriptors passed')
