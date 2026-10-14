"""Exercise the actual freestanding compare and its exported ABI wrapper."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
header=(root/'kernel/drivers/gpu/nvidia/vendor_abi/include/nvabi_internal.h').read_text()
source=(root/'kernel/drivers/gpu/nvidia/vendor_abi/nvabi_os.c').read_text()
def extract(text,marker):
    start=text.index(marker);end=text.index('\n}',start)+2
    return text[start:end]
code=r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#define NV_API_CALL
typedef int32_t NvS32;
'''+extract(header,'static inline int nvabi_string_compare(')+'\n'+extract(source,'NvS32 NV_API_CALL os_string_compare(')+r'''
static void compare(const char *a,const char *b) {
    int expected=strcmp(a,b);expected=expected<0 ? -1 : expected>0;
    assert(nvabi_string_compare(a,b)==expected && os_string_compare(a,b)==expected);
}
int main(void) {
    assert(!os_string_compare(NULL,NULL));
    assert(os_string_compare(NULL,"")==1 && os_string_compare("",NULL)==1);
    const char *values[]={"","a","ab","abc","abd","nv_post_event","nv_post_event_extra","nv_post_even","\x80","\xff"};
    for(unsigned i=0;i<sizeof(values)/sizeof(*values);++i)
        for(unsigned j=0;j<sizeof(values)/sizeof(*values);++j)compare(values[i],values[j]);
    char a[8193],b[8193];
    memset(a,'x',8192);memset(b,'x',8192);a[8192]=b[8192]=0;
    compare(a,b);b[0]='y';compare(a,b);b[0]='x';b[8191]='w';compare(a,b);
    uint32_t random=5070;
    for(unsigned n=0;n<10000;++n) {
        unsigned length=n%512;
        for(unsigned i=0;i<length;++i) {
            random=random*1664525u+1013904223u;a[i]=(char)(1+random%255);
            random=random*1664525u+1013904223u;b[i]=(char)(1+random%255);
        }
        a[length]=b[length]=0;compare(a,b);
    }
    size_t page=(size_t)sysconf(_SC_PAGESIZE);
    char *guard=mmap(NULL,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(guard!=MAP_FAILED && !mprotect(guard+page,page,PROT_NONE));
    char *end=guard+page-4;memcpy(end,"abc",4);
    compare(end,"abc");compare(end,"abcdef");compare("ab",end);compare(end,"abd");
    assert(!munmap(guard,page*2));
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='nvidia-string-compare-') as directory:
    base=Path(directory);(base/'test.c').write_text(code)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    str(base/'test.c'),'-o',str(base/'test')],check=True)
    subprocess.run([str(base/'test')],check=True,timeout=10)
print('NVIDIA single-pass string compare: actual ABI wrapper, null compatibility, unsigned bytes, prefixes, long strings, 10000 pairs and guard pages passed')
