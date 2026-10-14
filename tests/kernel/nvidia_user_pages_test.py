"""Run production imported-user-page ABI functions without host GPU access."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/drivers/gpu/nvidia/vendor_abi/nvabi_memory.c').read_text()
begin = source.index('NV_STATUS NV_API_CALL os_lock_user_pages(')
end = source.index('static uint32_t nvabi_cache_type(', begin)
code = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <assert.h>
#define NV_API_CALL
#define PAGE_SIZE 4096ULL
#define NVABI_PAGE_MASK 4095ULL
#define NVABI_PAGE_ALLOCATION_MAGIC 0x4e564d45U
#define NV_MEMORY_CACHED 1
#define NV_PORT_CACHE_WB 1
#define NV_OK 0
#define NV_ERR_INVALID_ARGUMENT 1
#define NV_ERR_NOT_SUPPORTED 2
#define NV_ERR_NO_MEMORY 3
#define NV_TRUE 1
#define NV_FALSE 0
#define K_OK 0
#define FLD_TEST_DRF(a,b,c,d,f) ((f)&1U)
typedef uint64_t NvU64,vaddr_t;
typedef uint32_t NvU32,NV_STATUS;
typedef bool NvBool;
typedef int nv_state_t,kstatus_t;
typedef struct {unsigned refs;} page_t;
typedef struct {int unused;} vm_space_t;
typedef struct {vm_space_t *vm;} process_t;
typedef struct {process_t *process;} thread_t;
typedef struct {uint64_t phys_pages[2];bool contiguous;bool owns_pages;} nv_port_mem_t;
typedef struct {unsigned magic,page_count,cache_type;NvBool contiguous;
 nv_port_mem_t memory;page_t **imported_user_pages;void *import_private;} nvabi_page_allocation_t;
static vm_space_t space;
static process_t process={&space};
static thread_t thread={&process};
static page_t pages[2]={{1},{1}};
static unsigned allocs,frees,pins,unpins,wraps,mem_frees,invalid;
static bool no_thread,no_alloc,write_expected;
static int fail_page=-1,wrap_status;
static thread_t *sched_current_thread(void) {return no_thread?NULL:&thread;}
static void *kmalloc(size_t n,unsigned f) {(void)f;if(no_alloc)return NULL;++allocs;return malloc(n);}
static void *kzalloc(size_t n,unsigned f) {void *p=kmalloc(n,f);if(p){unsigned char *b=p;for(size_t i=0;i<n;++i)b[i]=0;}return p;}
static void kfree(void *p) {if(p){++frees;free(p);}}
static bool nv_port_user_range_valid(void *p,size_t n) {return (uintptr_t)p>=0x10000 && (uintptr_t)p<0x800000000000ULL && n<=0x800000000000ULL-(uintptr_t)p;}
static kstatus_t vm_pin_user_page(vm_space_t *s,vaddr_t a,bool write,page_t **out) {
 assert(s==&space && write==write_expected && a>=0x10000 && a<0x12000);
 unsigned i=(unsigned)((a-0x10000)/PAGE_SIZE);++pins;
 if((int)i==fail_page)return 7;
 *out=&pages[i];++pages[i].refs;return 0;
}
static void vm_unpin_user_page(page_t *p) {assert(p->refs>1);--p->refs;++unpins;}
static NV_STATUS nvabi_status_from_kstatus(kstatus_t s) {return (NV_STATUS)s;}
static kstatus_t nv_port_mem_wrap_pages(page_t **p,uint32_t count,size_t n,uint32_t cache,nv_port_mem_t *out) {
 assert(p[0]==&pages[0] && p[1]==&pages[1] && count==2 && n==8192 && cache==NV_PORT_CACHE_WB);++wraps;
 if(wrap_status)return wrap_status;
 out->phys_pages[0]=0x45000;out->phys_pages[1]=0x78000;out->owns_pages=false;return 0;
}
static void nv_port_mem_free(nv_port_mem_t *m) {assert(!m->owns_pages);++mem_frees;}
static void nvabi_log_invalid(const char *a,const char *b) {assert(a && b);++invalid;}
''' + source[begin:end] + r'''
int main(void) {
 for(unsigned write=0;write<2;++write) {
  write_expected=write;void *array=NULL;unsigned old=allocs;
  assert(os_lock_user_pages((void*)0x10000,2,&array,write)==0 && array && allocs==old+1);
  assert(pages[0].refs==2 && pages[1].refs==2);
  uint64_t phys[2]={0};void *private_data=array;int token=42;
  assert(nv_register_user_pages(NULL,2,phys,&token,&private_data,false)==0 && private_data!=array);
  assert(phys[0]==0x45000 && phys[1]==0x78000);
  void *import=NULL;nv_unregister_user_pages(NULL,2,&import,&private_data);
  assert(import==&token && private_data==array && pages[0].refs==2);
  assert(os_unlock_user_pages(2,array,write)==0 && pages[0].refs==1 && pages[1].refs==1);
 }
 write_expected=true;fail_page=1;void *array=(void*)1;
 unsigned old=unpins;
 assert(os_lock_user_pages((void*)0x10000,2,&array,1)==7 && !array && unpins==old+1 && pages[0].refs==1);
 fail_page=-1;no_alloc=true;
 assert(os_lock_user_pages((void*)0x10000,2,&array,1)==NV_ERR_NO_MEMORY && !array);
 no_alloc=false;no_thread=true;
 assert(os_lock_user_pages((void*)0x10000,2,&array,1)==NV_ERR_NOT_SUPPORTED && !array);
 no_thread=false;
 assert(os_lock_user_pages((void*)0x10001,2,&array,1)==NV_ERR_INVALID_ARGUMENT && !array);
 assert(os_lock_user_pages((void*)0x10000,UINT64_MAX,&array,1)==NV_ERR_INVALID_ARGUMENT && !array);
 assert(os_lock_user_pages((void*)0x10000,0,&array,1)==NV_ERR_INVALID_ARGUMENT && !array);
 assert(os_lock_user_pages((void*)0x10000,2,NULL,1)==NV_ERR_INVALID_ARGUMENT);
 assert(os_lock_user_pages((void*)0x10000,2,&array,1)==0);
 void *private_data=array;uint64_t phys[2];wrap_status=8;
 assert(nv_register_user_pages(NULL,2,phys,NULL,&private_data,false)==8 && private_data==array);
 assert(nv_register_user_pages(NULL,2,phys,NULL,&private_data,true)==NV_ERR_NOT_SUPPORTED);
 wrap_status=0;assert(os_unlock_user_pages(2,array,1)==0);
 nv_unregister_user_pages(NULL,2,NULL,NULL);assert(invalid==1);
 assert(allocs==frees && pages[0].refs==1 && pages[1].refs==1 && wraps==3 && mem_frees==2);
}
'''
with tempfile.TemporaryDirectory(prefix='nv-user-pages-') as directory:
    base=Path(directory)
    (base/'test.c').write_text(code)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    str(base/'test.c'),'-o',str(base/'test')],check=True)
    subprocess.run([str(base/'test')],check=True)
print('NVIDIA imported user pages: read/write pins, partial rollback, registration/restore ownership, WB aliases, errors and balanced cleanup passed')
