"""Run the actual kernel FX state switch over all 16 XMM registers and MXCSR."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/arch/x86_64/cpu/cpu.c').read_text()
def function(name):
    start = source.index(name)
    start = source.rfind('\n', 0, start)+1
    return source[start:source.index('\n}',start)+2]
stores = '\n'.join(f'"movdqu %%xmm{i}, {i*16}(%0)\\n\\t"' for i in range(16))
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <kernel/string.h>
typedef struct { void *xsave_area; uint32_t xsave_size; } arch_thread_state_t;
#define X86_FX_STATE_SIZE 512U
''' + function('static void x86_fp_default_state(') + '\n' + function('void x86_fp_switch(') + r'''
static uint8_t original[512] __attribute__((aligned(16)));
static uint8_t a[512] __attribute__((aligned(16)));
static uint8_t b[512] __attribute__((aligned(16)));
static uint8_t observed[256] __attribute__((aligned(16)));
static void snapshot(void) {
    __asm__ volatile (
''' + stores + r'''
        : : "r"(observed) : "memory");
}
int main(void) {
    __asm__ volatile ("fxsave64 (%0)" : : "r"(original) : "memory");
    x86_fp_default_state(a); x86_fp_default_state(b);
    for(unsigned i=0;i<256;i++) {
        a[160+i]=(uint8_t)(i*3+7); b[160+i]=(uint8_t)(i*5+11);
    }
    *(uint32_t *)(a+24)=0x3f80; *(uint32_t *)(b+24)=0x5f80;
    arch_thread_state_t sa={a,512},sb={b,512};
    __asm__ volatile ("fxrstor64 (%0)" : : "r"(a) : "memory");
    for(unsigned iteration=0;iteration<1000;iteration++) {
        x86_fp_switch(&sa,&sb); snapshot();
        uint32_t mxcsr;
        __asm__ volatile ("stmxcsr %0" : "=m"(mxcsr));
        assert(mxcsr==0x5f80);
        for(unsigned i=0;i<256;i++) assert(observed[i]==(uint8_t)(i*5+11));
        x86_fp_switch(&sb,&sa); snapshot();
        __asm__ volatile ("stmxcsr %0" : "=m"(mxcsr));
        assert(mxcsr==0x3f80);
        for(unsigned i=0;i<256;i++) assert(observed[i]==(uint8_t)(i*3+7));
    }
    __asm__ volatile ("fxrstor64 (%0)" : : "r"(original) : "memory");
    puts("Production FX switch: XMM0-15 and MXCSR preserved over 2000 switches PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-fxsave-xmm-') as directory:
    path = Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc','-O2','-std=c11','-Wall','-Wextra','-Werror',
                    '-ffreestanding','-fno-builtin','-mno-sse','-mno-mmx',
                    '-I'+str(root/'include'),str(path/'test.c'),
                    str(root/'kernel/string.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
