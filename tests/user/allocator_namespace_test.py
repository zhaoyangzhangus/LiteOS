"""Canonical allocator hooks retire old exports without changing allocation."""
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[2]
source=(ROOT/'user/libc/alloc.c').read_text()
legacy=source
for verifier in ('tools/verify-libc.sh','tools/verify-libc.ps1'):
    text=(ROOT/verifier).read_text()
    for name in ('free','realloc','malloc_usable_size'):
        assert '__system_'+name in text and '__liteos_system_'+name not in text
for name in ('free','realloc','malloc_usable_size'):
    legacy=legacy.replace('__system_'+name,'__liteos_system_'+name)

program=r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <uapi/mm.h>
#include <uapi/syscall.h>
static bool linux_mode;
static unsigned maps, unmaps;
bool __libc_linux_personality(void) { return linux_mode; }
int64_t __libc_native_syscall6(uint64_t number,uint64_t a,uint64_t b,
                        uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
    assert(!c && !d && !e && !f);
    if (number==OS_SYS_VM_MAP) {
        os_vm_map_args_t *request=(void *)(uintptr_t)a;
        assert(!b && request->hdr.size==sizeof(*request));
        void *p=mmap(0,request->length,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if (p==MAP_FAILED) return -ENOMEM;
        request->address=(uintptr_t)p; ++maps; return 0;
    }
    assert(number==OS_SYS_VM_UNMAP); ++unmaps;
    return munmap((void *)(uintptr_t)a,b);
}
void *tested_realloc(void *,size_t);
#define malloc tested_malloc
#define calloc tested_calloc
#define free tested_free
#define realloc tested_realloc
#define reallocarray tested_reallocarray
#define aligned_alloc tested_aligned_alloc
#define posix_memalign tested_posix_memalign
#define malloc_usable_size tested_usable_size
''' + source.replace('#include "libc/libc.h"','') + r'''
#undef malloc
#undef calloc
#undef free
#undef realloc
int main(void) {
    for (unsigned mode=0; mode<2; ++mode) {
        linux_mode=mode;
        for (unsigned round=0; round<2; ++round) {
            void (*release)(void *)=__system_free;
            void *(*resize)(void *,size_t)=__system_realloc;
            size_t (*usable)(void *)=__system_malloc_usable_size;
            release(0); assert(!usable(0));
            unsigned char *p=resize(0,17); assert(p && usable(p)==17);
            memset(p,0x59,17);
            unsigned char *grown=resize(p,8192); assert(grown && usable(grown)==8192);
            for (unsigned i=0; i<17; ++i) assert(grown[i]==0x59);
            assert(resize(grown,9)==grown && usable(grown)==9);
            assert(!resize(grown,0));
            p=tested_calloc(64,1); assert(p);
            for (unsigned i=0; i<64; ++i) assert(!p[i]);
            release(p);
            errno=0; assert(!resize(0,SIZE_MAX) && errno==EINVAL);
        }
    }
    assert(maps==unmaps && maps==6);
}
'''
with tempfile.TemporaryDirectory(prefix='allocator-namespace-') as directory:
    base=Path(directory)
    for name,text in (('new',source),('legacy',legacy)):
        c=base/(name+'.c'); c.write_text(text)
        obj=base/(name+'.o')
        subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin',
                        '-fPIC','-I'+str(ROOT/'include'),'-I'+str(ROOT/'user/libc/include'),
                        '-c',str(c),'-o',str(obj)],check=True)
        subprocess.run(['objcopy','-O','binary','--only-section=.text',str(obj),str(base/(name+'.text'))],check=True)
    assert (base/'new.text').read_bytes()==(base/'legacy.text').read_bytes()
    symbols=subprocess.check_output(['nm','-g','--defined-only',str(base/'new.o')],text=True)
    table={row.split()[2]:row.split()[0] for row in symbols.splitlines()}
    for name in ('free','realloc','malloc_usable_size'):
        assert '__system_'+name in table and '__liteos_system_'+name not in table
    old_symbols=subprocess.check_output(['nm','-g','--defined-only',str(base/'legacy.o')],text=True)
    old_names={row.split()[2] for row in old_symbols.splitlines()}
    normalized_old={name.replace('__liteos_system_', '__system_') for name in old_names}
    assert set(table)==normalized_old
    c=base/'behavior.c'; c.write_text(program)
    exe=base/'behavior'
    subprocess.run(['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-fno-builtin',
                    '-I'+str(ROOT/'include'),str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('Allocator hooks: retired exports absent, identical code and native/Linux allocation behavior passed')
