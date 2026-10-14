"""Production space creation and actual kernel bulk copy, with failure injection."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "kernel/mm/vm_space.c").read_text()
begin = source.index("kstatus_t vm_space_create(")
end = source.index("void vm_space_get(", begin)
creation = source[begin:end].replace("memcpy(", "root_copy(")
program = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel/vm.h>
static atomic_uint_fast32_t g_next_pcid;
static unsigned failure, initializes, allocations, pages, space_frees, page_frees, copies;
static page_t root_page;
static struct {uint64_t before[2],entries[512],after[2];} root_table;
static uint64_t old_root[512],saved_root[512];
static void vm_global_initialize(void) { ++initializes; }
void *kzalloc(size_t size,uint32_t flags) {
 assert(size==sizeof(vm_space_t) && !flags);++allocations;
 return failure==1?0:calloc(1,size);
}
void kfree(void *space) {assert(space);++space_frees;free(space);}
page_t *page_alloc(uint8_t order,page_alloc_flags_t flags) {
 assert(!order && flags==(PAGE_ALLOC_ZERO|PAGE_ALLOC_DMA32));++pages;
 return failure==2?0:&root_page;
}
void page_free(page_t *page) {assert(page==&root_page);++page_frees;}
paddr_t page_to_phys(const page_t *page) {assert(page==&root_page);return (paddr_t){PAGE_SIZE};}
static paddr_t x86_current_root_table(void) {return (paddr_t){2*PAGE_SIZE};}
void *phys_to_direct(paddr_t physical) {
 if(physical.value==PAGE_SIZE)return failure==3?0:root_table.entries;
 assert(physical.value==2*PAGE_SIZE);return failure==4?0:old_root;
}
static void *root_copy(void *destination,const void *source,size_t size) {
 assert(destination==root_table.entries+256 && source==old_root+256 && size==2048);
 ++copies;return memcpy(destination,source,size);
}
''' + creation + r'''
int main(void) {
 const uint32_t pcids[]={1,4094,4095,65535};
 for(unsigned round=0;round<128;++round)for(unsigned f=0;f<5;++f) {
  failure=f;initializes=allocations=pages=space_frees=page_frees=copies=0;
  memset(&root_page,0,sizeof(root_page));memset(root_table.entries,0,sizeof(root_table.entries));
  root_table.before[0]=root_table.before[1]=root_table.after[0]=root_table.after[1]=0xabcddcba01234567ULL;
  for(unsigned i=0;i<512;++i)old_root[i]=((uint64_t)(round+1)<<48)^((uint64_t)i*0x123456789abcdULL);
  memcpy(saved_root,old_root,sizeof(old_root));
  uint32_t pcid=pcids[round%4];atomic_store(&g_next_pcid,pcid);
  vm_space_t *out=(void*)1;kstatus_t status=vm_space_create(&out);
  assert(initializes==1 && allocations==1 && pages==(f!=1));
  assert(!memcmp(saved_root,old_root,sizeof(old_root)));
  for(unsigned i=0;i<2;++i)assert(root_table.before[i]==0xabcddcba01234567ULL && root_table.after[i]==0xabcddcba01234567ULL);
  for(unsigned i=0;i<256;++i)assert(!root_table.entries[i]);
  if(f) {
   assert(status==(f<=2?K_ENOMEM:K_EIO) && out==(void*)1 && !copies);
   assert(space_frees==(f!=1) && page_frees==(f>=3));
   assert(atomic_load(&g_next_pcid)==pcid);
   for(unsigned i=256;i<512;++i)assert(!root_table.entries[i]);
   continue;
  }
  assert(status==K_OK && copies==1 && !space_frees && !page_frees);
  for(unsigned i=256;i<512;++i)assert(root_table.entries[i]==old_root[i]);
  assert(atomic_load(&out->refs.value)==1 && out->root_table.value==PAGE_SIZE);
  assert(out->pcid==(pcid<=4094?pcid:0) && root_page.private_data==out->pcid);
  assert(root_page.owner==PAGE_OWNER_PAGETABLE && atomic_load(&g_next_pcid)==pcid+1);
  assert(!out->areas.root && out->area_list.next==&out->area_list && out->area_list.prev==&out->area_list);
  assert(!atomic_load(&out->map_lock.state));
  for(unsigned i=0;i<VM_PT_LOCK_COUNT;++i)assert(!atomic_load(&out->pt_locks[i].state));
  assert(atomic_load(&out->tlb_generation)==1 && atomic_load(&out->advice_generation)==1);
  assert(!atomic_load(&out->rss_pages) && !atomic_load(&out->commit_pages));
  page_free(&root_page);kfree(out);
 }
 initializes=allocations=pages=0;
 assert(vm_space_create(0)==K_EINVAL && !initializes && !allocations && !pages);
 puts("VM space creation: 640 success/OOM/direct-map cases, one actual 2048-byte kernel copy, exact inherited kernel half/empty user half/canaries, unchanged source, PCID exhaustion and lock/accounting initialization passed");
}
'''
with tempfile.TemporaryDirectory(prefix="system-space-create-") as directory:
    binary = str(Path(directory) / "test")
    subprocess.run([
        "cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-fno-builtin",
        "-mno-sse", "-mno-mmx", "-Dmemcpy=liteos_memcpy",
        "-Dmemset=liteos_memset", "-Dmemmove=liteos_memmove", "-Dmemcmp=liteos_memcmp",
        "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
        "-I"+str(root/"include"), "-x", "c", "-", str(root/"kernel/string.c"), "-o", binary,
    ], input=program, text=True, check=True)
    subprocess.run([binary], check=True, timeout=30)
