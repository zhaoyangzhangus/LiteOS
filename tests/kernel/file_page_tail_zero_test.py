"""Actual file read/cleanup/tail-zero block on poisoned allocation bytes."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/fs/vfs/page_cache.c').read_text()
start=source.index('    read_length = backend->size > file_offset')
end=source.index('    if (prefetch && bytes <= PAGE_SIZE)',start)
block=source[start:end].replace('memset(', 'liteos_memset(')
code=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define PAGE_SIZE 4096U
#define K_OK 0
#define K_EIO -5
typedef int kstatus_t;
void *liteos_memset(void *,int,size_t);
typedef struct { uint64_t size;void *context;
    kstatus_t (*read)(void *,uint64_t,void *,size_t,uint64_t *); } backend_t;
static uint8_t arena[8192+128];
static unsigned reads,frees,entries,unlocks,depth,mode;
static void vfs_backend_lock(void *p) {(void)p;assert(!depth);++depth;}
static void vfs_backend_unlock(void *p) {(void)p;assert(depth==1);--depth;}
static void page_free(void *p) {assert(p==arena+64);++frees;}
static void kfree(void *p) {if(p)++entries;}
static void vfs_vnode_unlock(void *p) {assert(p);++unlocks;}
static kstatus_t read_data(void *context,uint64_t offset,void *out,size_t n,uint64_t *bytes) {
    (void)context;assert(depth==1 && out==arena+64 && n<=8192);++reads;
    for(size_t i=0;i<n;++i)((uint8_t *)out)[i]=(uint8_t)((offset+i)*17+3);
    *bytes=mode==1?n-1:mode==2?n+1:n;
    return mode==3?-12:K_OK;
}
static kstatus_t fill(backend_t *backend,uint64_t file_offset,bool prefetch) {
    uint8_t *memory=arena+64;void *page=memory,*entry=backend,*next_entry=prefetch?backend:NULL,*vnode=backend;
    uint64_t read_length,bytes=0;
''' + block + r'''
    return K_OK;
}
int main(void) {
    unsigned cases=0;
    backend_t backend={.read=read_data};
    for(unsigned prefetch=0;prefetch<2;++prefetch) {
        unsigned capacity=prefetch?8192:4096;
        for(unsigned length=0;length<=capacity;++length)for(unsigned alignment=0;alignment<2;++alignment) {
            uint64_t offset=alignment?17:0;
            backend.size=offset+length;
            memset(arena,0x75,sizeof(arena));reads=frees=entries=unlocks=0;mode=0;
            assert(fill(&backend,offset,prefetch)==K_OK);
            assert(reads==(length!=0) && !frees && !entries && !unlocks && !depth);
            for(unsigned i=0;i<capacity;++i)
                assert(arena[64+i]==(i<length?(uint8_t)((offset+i)*17+3):0));
            for(unsigned i=0;i<64;++i)
                assert(arena[i]==0x75 && arena[64+capacity+i]==0x75);
            ++cases;
        }
        for(mode=1;mode<=3;++mode) {
            backend.size=37;memset(arena,0x75,sizeof(arena));
            reads=frees=entries=unlocks=0;
            assert(fill(&backend,0,prefetch)==(mode==3?-12:K_EIO));
            assert(reads==1 && frees==1 && entries==1+prefetch && unlocks==1 && !depth);
            for(unsigned i=37;i<capacity;++i)assert(arena[64+i]==0x75);
        }
    }
    printf("File tail: %u exact EOF/full-page/unaligned-offset poison cases; read failures rejected before clearing/publication PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-file-tail-') as directory:
    path=Path(directory);obj=str(path/'string.o');binary=str(path/'test')
    flags=['-std=c11','-O2','-Wall','-Wextra','-Werror','-fno-builtin',
           '-mno-sse','-mno-mmx','-I'+str(root/'include'),
           '-fsanitize=address,undefined','-fno-pie']
    subprocess.run(['cc',*flags,'-Dmemset=liteos_memset','-Dmemcpy=liteos_memcpy',
                    '-Dmemmove=liteos_memmove','-Dmemcmp=liteos_memcmp',
                    '-c',str(root/'kernel/string.c'),'-o',obj],check=True)
    subprocess.run(['cc',*flags,'-no-pie','-x','c','-','-x','none',obj,'-o',binary],
                   input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
