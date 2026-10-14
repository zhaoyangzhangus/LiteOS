"""Execute production immediate-allocation mmap registration and failure paths."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/drivers/gpu/nvidia/rm/nv_rm_vendor_bridge.c').read_text()
start = source.index('    case NV_ESC_RM_ALLOC_MEMORY: {')
end = source.index('    case NV_ESC_RM_ALLOC_CONTEXT_DMA2:',start)
body = source[start:end]
code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#define NV_ESC_RM_ALLOC_MEMORY 0x27
#define NV01_MEMORY_SYSTEM 0x3e
#define NV_MEMORY_DEFAULT 6
#define NV_OK 0
#define K_OK 0
#define K_EINVAL -22
typedef uint32_t NvU32;
typedef struct {int unused;} API_SECURITY_INFO;
typedef struct {uint32_t hClass,flags,status,hRoot,hObjectParent,hObjectNew;
                uint64_t pMemory,limit;} NVOS02_PARAMETERS;
typedef struct {NVOS02_PARAMETERS params; int fd;} nv_ioctl_nvos02_parameters_with_fd;
typedef struct {bool control; uint32_t minor;} file_t;
#define FIELD_ALLOC 16
#define FIELD_MAPPING 30
#define FLD_TEST_DRF(os,flags,field,value,v) (((v)>>FIELD##field&3)==1)
static unsigned allocations,mappings; static uint32_t alloc_status,map_status;
static nv_ioctl_nvos02_parameters_with_fd api;
static void nv_rm_vendor_frontend_security(file_t *f,API_SECURITY_INFO *s) {(void)f;(void)s;}
static void Nv01AllocMemoryWithSecInfo(NVOS02_PARAMETERS *p,API_SECURITY_INFO s) {
    (void)s; assert(p==&api.params); ++allocations; p->status=alloc_status;
    p->flags=0; /* Must inspect original caller flags, not RM's output. */
}
static uint32_t rm_create_mmap_context(uint32_t c,uint32_t d,uint32_t m,
    uint64_t a,uint64_t n,uint64_t o,uint32_t cache,uint32_t fd) {
    assert(c==11 && d==12 && m==13 && a==0x12345000 && n==4096 && !o && cache==6 && fd==19);
    ++mappings; return map_status;
}
static int dispatch(file_t *file,unsigned request,void *argument,unsigned argument_size) {
    switch(request) {
''' + body + r'''
    default:return -38;
    }
}
static void setup(unsigned flags,unsigned cls) {
    api=(nv_ioctl_nvos02_parameters_with_fd){{cls,flags,0,11,12,13,0x12345000,4095},19};
    allocations=mappings=alloc_status=map_status=0;
}
int main(void) {
    file_t f={false,0};
    setup(0,0x3e); assert(!dispatch(&f,0x27,&api,sizeof(api)) && allocations==1 && mappings==1);
    unsigned skips[]={1U<<16,1U<<30};
    for(unsigned i=0;i<2;++i) {setup(skips[i],0x3e); assert(!dispatch(&f,0x27,&api,sizeof(api)) && !mappings);}
    setup(0,0x40); assert(!dispatch(&f,0x27,&api,sizeof(api)) && !mappings);
    setup(0,0x3e); alloc_status=7; assert(!dispatch(&f,0x27,&api,sizeof(api)) && !mappings);
    setup(0,0x3e); map_status=7; assert(dispatch(&f,0x27,&api,sizeof(api))==-22 && mappings==1);
    setup(0,0x3e); f.control=true; assert(dispatch(&f,0x27,&api,sizeof(api))==-22 && !allocations);
    f.control=false; assert(dispatch(&f,0x27,&api,sizeof(api)-1)==-22 && !allocations);
}
'''
with tempfile.TemporaryDirectory(prefix='nvidia-alloc-mmap-') as directory:
    base=Path(directory)
    (base/'test.c').write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(base/'test.c'),'-o',str(base/'test')],check=True)
    subprocess.run([str(base/'test')],check=True)
print('NVIDIA immediate allocation: real adapter registers exact fd, original flags, allocation/map failures and validation passed')
