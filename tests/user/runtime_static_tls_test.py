"""Execute loader TLS layout/allocation and libc child-template cloning."""
from pathlib import Path
import re
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
loader = (root/'user/runtime/ld.c').read_text()
pthread = (root/'user/libc/pthread.c').read_text()


def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


macros = '\n'.join(re.findall(r'^#define OS_RT_.*', loader, re.M))
types = loader[loader.index('typedef struct loaded_image {'):loader.index('typedef struct aux_value {')]
code = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <uapi/syscall.h>
#include <uapi/process.h>
#include <uapi/mm.h>
#include <uapi/linux_abi.h>
#include "elf64.h"
''' + macros + '\n' + types + r'''
#define LIBC_TLS_MAGIC OS_RT_TLS_MAGIC
#define LIBC_TLS_HEADER_WORDS OS_RT_TLS_HEADER_WORDS
#define PTHREAD_PAGE_SIZE 4096U
typedef struct {void *tls_mapping;size_t tls_mapping_size;uint64_t tls_fs_base;} fixture_thread;
typedef fixture_thread *fixture_thread_t;
static uint64_t current_fs;
static bool fail_context;
static unsigned allocations, frees, fail_allocation;
static void *mappings[8];
static bool map_anonymous_any(uint64_t size, uint32_t protection, uint64_t *address) {
    assert(protection == (OS_VM_READ | OS_VM_WRITE));
    if (++allocations == fail_allocation) return false;
    void *p = malloc(size);assert(p);
    memset(p,0xab,size);mappings[allocations-1]=p;*address=(uintptr_t)p;return true;
}
static int64_t runtime_syscall3(uint64_t number,uint64_t address,uint64_t size,uint64_t unused) {
    assert(number == OS_SYS_VM_UNMAP && size && !unused);
    for(unsigned i=0;i<allocations;i++)if(mappings[i]==(void *)(uintptr_t)address){
        free(mappings[i]);mappings[i]=0;++frees;return 0;
    }
    assert(false);return -1;
}
static int64_t runtime_syscall6(uint64_t number,uint64_t operation,uint64_t address,
                                uint64_t a,uint64_t b,uint64_t c,uint64_t d) {
    assert(number==OS_SYS_THREAD_CONTEXT && operation==OS_THREAD_CONTEXT_SET_FS && !a&&!b&&!c&&!d);
    if(fail_context)return -1;
    current_fs=address;return 0;
}
static int64_t __libc_linux_syscall6(uint64_t number,uint64_t operation,uint64_t address,
                                     uint64_t a,uint64_t b,uint64_t c,uint64_t d) {
    assert(number==LINUX_SYS_ARCH_PRCTL && operation==LINUX_ARCH_GET_FS && !a&&!b&&!c&&!d);
    *(uint64_t *)(uintptr_t)address=current_fs;return 0;
}
static void reset(void){for(unsigned i=0;i<8;i++){free(mappings[i]);mappings[i]=0;}
    allocations=frees=fail_allocation=0;fail_context=false;current_fs=0;}
''' + '\n'.join(function(loader, name) for name in
                ('static bool add_u64(', 'static bool align_up_u64(',
                 'static void copy_bytes(', 'static void zero_bytes(',
                 'static bool pointer_range(', 'static bool setup_main_tls(',
                 'static bool plan_static_tls(', 'static bool setup_static_tls('))
code += '\n' + '\n'.join(function(pthread, name).replace('pthread_t', 'fixture_thread_t')
                         for name in ('static size_t page_round(',
                                      'static bool pthread_tls_metadata(',
                                      'static bool pthread_prepare_tls('))
code += r'''
static void module(loaded_image_t *image,uint8_t *bytes,unsigned size,unsigned initialized,unsigned alignment){
    memset(image,0,sizeof(*image));image->image_start=(uintptr_t)bytes;image->image_end=(uintptr_t)bytes+size;
    image->phdr_count=1;image->phdrs[0]=(runtime_elf64_program_header_t){
        .type=OS_RT_PT_TLS,.virtual_address=(uintptr_t)bytes,.memory_size=size,.file_size=initialized,.alignment=alignment};
}
static void verify_module(loaded_image_t *image,uint64_t fs){
    if(!image->tls_size)return;
    uint8_t *p=(uint8_t *)(uintptr_t)(fs-image->tls_offset);
    assert((uintptr_t)p%image->tls_alignment==image->tls_source%image->tls_alignment);
    assert(!memcmp(p,(void *)(uintptr_t)image->tls_source,image->tls_file_size));
    for(unsigned i=image->tls_file_size;i<image->tls_size;i++)assert(!p[i]);
}
int main(void){
    _Alignas(128) uint8_t bytes[3][256];memset(bytes,0x56,sizeof(bytes));
    loaded_image_t images[3];uint64_t total,alignment;
    for(unsigned phase=0;phase<64;phase++){
        module(&images[0],bytes[0],31,7,16);
        module(&images[1],bytes[1]+phase,13,1,64);
        module(&images[2],bytes[2]+3,17,4,8);
        assert(plan_static_tls(images,3,&total,&alignment));
        assert(images[0].tls_offset==32&&alignment==64&&total%alignment==0);
        for(unsigned i=1;i<3;i++)assert(images[i].tls_offset-images[i].tls_size>=images[i-1].tls_offset);
        assert(setup_static_tls(images,3,total,alignment));
        uint64_t *tcb=(void *)(uintptr_t)current_fs;
        assert(tcb[0]==current_fs&&tcb[1]==total&&tcb[3]==OS_RT_TLS_MAGIC&&tcb[4]==0&&tcb[5]==alignment);
        for(unsigned i=0;i<3;i++)verify_module(&images[i],current_fs);
        fixture_thread child={0};assert(pthread_prepare_tls(&child));
        assert(child.tls_mapping&&child.tls_fs_base!=current_fs);
        for(unsigned i=0;i<3;i++)verify_module(&images[i],child.tls_fs_base);
        uint64_t *child_tcb=(void *)(uintptr_t)child.tls_fs_base;
        assert(child_tcb[0]==child.tls_fs_base&&child_tcb[4]==(uintptr_t)&child&&child_tcb[1]==total);
        *(uint8_t *)(uintptr_t)(child.tls_fs_base-images[0].tls_offset)=0;
        assert(*(uint8_t *)(uintptr_t)(current_fs-images[0].tls_offset)==0x56);
        munmap(child.tls_mapping,child.tls_mapping_size);reset();
    }
    module(&images[0],bytes[0],31,7,16);
    assert(plan_static_tls(images,1,&total,&alignment));assert(setup_static_tls(images,1,total,alignment));
    assert(((uint64_t *)(uintptr_t)current_fs)[1]==31);verify_module(&images[0],current_fs);reset();
    module(&images[0],bytes[0]+1,3,1,1);
    assert(plan_static_tls(images,1,&total,&alignment));assert(setup_static_tls(images,1,total,alignment));
    assert(current_fs%sizeof(uint64_t)==0);verify_module(&images[0],current_fs);reset();
    memset(&images[0],0,sizeof(images[0]));module(&images[1],bytes[1]+3,13,1,16);
    assert(plan_static_tls(images,2,&total,&alignment));assert(setup_static_tls(images,2,total,alignment));
    verify_module(&images[1],current_fs);reset();
    for(unsigned failure=1;failure<=3;failure++){
        assert(plan_static_tls(images,2,&total,&alignment));
        fail_allocation=failure<3?failure:0;fail_context=failure==3;
        assert(!setup_static_tls(images,2,total,alignment));assert(!current_fs);
        assert(frees==failure-1);for(unsigned i=0;i<8;i++)assert(!mappings[i]);reset();
    }
    assert(!plan_static_tls(images,0,&total,&alignment));
    assert(!plan_static_tls(images,OS_RT_MAX_IMAGES+1,&total,&alignment));
    images[1].phdrs[0].alignment=3;assert(!plan_static_tls(images,2,&total,&alignment));
    images[1].phdrs[0].alignment=16;images[1].phdrs[0].file_size=14;
    assert(!plan_static_tls(images,2,&total,&alignment));
    images[1].phdrs[0].file_size=1;images[1].phdrs[0].memory_size=(1U<<20)+1;
    assert(!plan_static_tls(images,2,&total,&alignment));
    module(&images[1],bytes[1],13,1,16);images[1].phdr_count=2;images[1].phdrs[1]=images[1].phdrs[0];
    assert(!plan_static_tls(images,2,&total,&alignment));
    images[1].phdr_count=OS_RT_MAX_PROGRAMS+1;assert(!plan_static_tls(images,2,&total,&alignment));
    memset(images,0,sizeof(images));assert(plan_static_tls(images,3,&total,&alignment));
    assert(!total&&setup_static_tls(images,3,total,alignment)&&!allocations&&!current_fs);
    module(&images[0],bytes[0],1U<<19,0,16);
    module(&images[1],bytes[1],(1U<<19)+1,0,16);
    assert(!plan_static_tls(images,2,&total,&alignment));
    images[1].phdrs[0].memory_size=1U<<19;
    assert(plan_static_tls(images,2,&total,&alignment)&&total==(1U<<20));
}
'''
with tempfile.TemporaryDirectory(prefix='system-static-tls-') as directory:
    base=Path(directory);(base/'test.c').write_text(code)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I',str(root/'include'),'-I',str(root/'user/runtime'),
                    str(base/'test.c'),'-o',str(base/'test')],check=True)
    subprocess.run([str(base/'test')],check=True,timeout=15)
print('Static TLS: 64 alignment phases, three modules, BSS, exact main ABI, child isolation, DSO-only TLS and allocation/context rollback passed')
