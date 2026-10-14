#!/usr/bin/env python3
"""Exercise production wide-memory routines with Chromium's enum layout."""
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
source = (root / 'user/libc/wchar.c').read_text()
start = source.index('wchar_t *wmemcpy(')
end = source.index('int mbsinit(', start)
routines = source[start:end]
code = r'''
#include <stddef.h>
#include <stdint.h>
#include <assert.h>
typedef __WCHAR_TYPE__ wchar_t;
'''+routines+r'''
int main(void) {
    const uint32_t states[]={7,2,3};
    wchar_t *found=wmemchr((const wchar_t*)states,3,3);
#ifdef OLD_SHORT_ABI
    assert(sizeof(wchar_t)==2 && found==0);
#else
    assert(sizeof(wchar_t)==4);
    assert(found==(wchar_t*)&states[2]);
    assert(!wmemchr((const wchar_t*)states,3,2));
    assert(!wmemchr((const wchar_t*)states,9,3));
    wchar_t copy[4]={0};
    wmemcpy(copy,(const wchar_t*)states,3);
    assert(!wmemcmp(copy,(const wchar_t*)states,3));
    wmemmove(copy+1,copy,3);
    assert(copy[1]==7 && copy[2]==2 && copy[3]==3);
    wmemset(copy,0x1f600,4);
    for (int i=0;i<4;++i) assert(copy[i]==0x1f600);
    assert(!wmemchr(copy,0x1f600,0));
#endif
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-wchar-abi-test-') as directory:
    for old in (True, False):
        binary=str(pathlib.Path(directory)/('old' if old else 'fixed'))
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fno-builtin',
                        *(['-fshort-wchar','-DOLD_SHORT_ABI'] if old else
                          ['-include',str(root/'user/runtime/elf_abi.h')]),
                        '-x','c','-','-o',binary],input=code,text=True,check=True)
        subprocess.run([binary],check=True)
print('ELF wchar: old Chromium enum-search failure reproduced; 32-bit routines passed')
