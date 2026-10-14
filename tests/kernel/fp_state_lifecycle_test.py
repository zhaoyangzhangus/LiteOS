"""Exercise production FP allocation/default/clone/destroy without privileged code."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/arch/x86_64/cpu/cpu.c').read_text()
def function(name):
    start = source.rfind('\n', 0, source.index(name)) + 1
    return source[start:source.index('\n}', start) + 2]

functions = '\n'.join(function(name) for name in (
    'static void x86_fp_default_state(', 'bool x86_fp_state_create(',
    'void x86_fp_state_destroy(', 'void x86_fp_state_clone_current('))
program = r'''
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <arch/x86_64/context.h>
#define X86_FX_STATE_SIZE 512U
void *liteos_memset(void *,int,size_t);
void *liteos_memmove(void *,const void *,size_t);
static _Alignas(16) unsigned char arena[544];
static unsigned mode,allocations,frees,depth;
void *kmalloc(size_t size,uint32_t flags) {
    assert(size==512 && flags==0); ++allocations;
    memset(arena,0x75,sizeof(arena));
    return mode==1?NULL:arena+16+(mode==2);
}
void kfree(void *area) {
    if(area) { assert(area==arena+16+(mode==2)); ++frees; }
}
void sched_preempt_disable(void) { ++depth; }
void sched_preempt_enable(void) { assert(depth); --depth; }
''' + functions.replace('memset(', 'liteos_memset(').replace('memmove(', 'liteos_memmove(') + r'''
int main(void) {
    arch_thread_state_t state={0},before;
    assert(!x86_fp_state_create(NULL) && allocations==0);
    state.xsave_area=arena;before=state;
    assert(!x86_fp_state_create(&state) && allocations==0);
    assert(!memcmp(&state,&before,sizeof(state)));
    for(mode=1;mode<=2;++mode) {
        memset(&state,0,sizeof(state));state.xsave_size=17;before=state;
        assert(!x86_fp_state_create(&state));
        assert(!memcmp(&state,&before,sizeof(state)));
        for(unsigned i=0;i<sizeof(arena);++i)assert(arena[i]==0x75);
    }
    assert(allocations==2 && frees==1);mode=0;
    for(unsigned iteration=0;iteration<1000;++iteration) {
        memset(&state,0,sizeof(state));assert(x86_fp_state_create(&state));
        assert(state.xsave_area==arena+16 && state.xsave_size==512);
        unsigned char expected[512]={0};
        expected[0]=0x7f;expected[1]=3;expected[24]=0x80;expected[25]=0x1f;
        expected[28]=0xff;expected[29]=0xff;
        assert(!memcmp(state.xsave_area,expected,sizeof(expected)));
        for(unsigned i=0;i<16;++i)assert(arena[i]==0x75 && arena[528+i]==0x75);
        x86_fp_state_destroy(&state);
        assert(!state.xsave_area && !state.xsave_size);
        unsigned saved=frees;x86_fp_state_destroy(&state);assert(frees==saved);
    }
    x86_fp_state_destroy(NULL);
    _Alignas(16) unsigned char original[512],saved_state[512],destination[544];
    __asm__ volatile("fxsave64 (%0)"::"r"(original):"memory");
    arch_thread_state_t from={0},to={0};
    from.xsave_area=saved_state;from.xsave_size=512;
    to.xsave_area=destination+16;to.xsave_size=512;
    for(unsigned iteration=0;iteration<1000;++iteration) {
        memset(saved_state,0x59,sizeof(saved_state));
        memset(destination,0x75,sizeof(destination));
        x86_fp_state_clone_current(&from,&to);
        assert(depth==0 && !memcmp(saved_state,destination+16,512));
        for(unsigned i=0;i<16;++i)assert(destination[i]==0x75 && destination[528+i]==0x75);
        x86_fp_state_clone_current(&from,&from);assert(depth==0);
    }
    for(unsigned invalid=0;invalid<6;++invalid) {
        arch_thread_state_t bad_from=from,bad_to=to;
        if(invalid==2)bad_from.xsave_area=NULL;
        if(invalid==3)bad_to.xsave_area=NULL;
        if(invalid==4)bad_from.xsave_size=511;
        if(invalid==5)bad_to.xsave_size=511;
        memset(destination,0x75,sizeof(destination));
        x86_fp_state_clone_current(invalid==0?NULL:&bad_from,invalid==1?NULL:&bad_to);
        assert(depth==0);
        for(unsigned i=0;i<sizeof(destination);++i)assert(destination[i]==0x75);
    }
    __asm__ volatile("fxrstor64 (%0)"::"r"(original):"memory");
    puts("FP lifecycle: 1000 poison/default/create/destroy cases, 2000 clones, failure/alignment/invalid checks PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-fp-state-') as directory:
    path=Path(directory)
    flags=['-std=c11','-O2','-Wall','-Wextra','-Werror','-fno-builtin',
           '-mno-sse','-mno-mmx','-I'+str(root/'include'),
           '-fsanitize=address,undefined','-fno-pie']
    subprocess.run(['cc',*flags,'-Dmemset=liteos_memset','-Dmemmove=liteos_memmove',
                    '-Dmemcpy=liteos_memcpy','-Dmemcmp=liteos_memcmp','-c',
                    str(root/'kernel/string.c'),'-o',str(path/'string.o')],check=True)
    subprocess.run(['cc',*flags,'-no-pie','-x','c','-','-x','none',
                    str(path/'string.o'),'-o',str(path/'test')],
                   input=program,text=True,check=True)
    subprocess.run([str(path/'test')],check=True,timeout=30)
