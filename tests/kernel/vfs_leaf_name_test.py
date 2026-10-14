"""Actual stat leaf extraction: one-pass versus length/backward-scan oracle."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/vfs/core.c').read_text()
leaf = source[source.index('static void vfs_set_leaf_name('):source.index('static void vfs_fill_info(')]
scan = '''    size_t length = 0U;
    size_t start = 0U;
    if (path != 0) {
        while (length < VFS_PATH_LIMIT && path[length] != 0) {
            if (path[length] == '/') start = length + 1U;
            ++length;
        }
    }
'''
assert scan in leaf
old = leaf.replace('vfs_set_leaf_name(', 'previous_leaf(').replace(scan,
    '    size_t length = vfs_string_length(path);\n    size_t start = length;\n')
old = old.replace('    size_t count = length - start;',
    "    while (start > 0U && path[start - 1U] != '/') { --start; ++backward; }\n    size_t count = length - start;")
code = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#define VFS_PATH_LIMIT 256U
#define OS_FILE_NAME_MAX 64U
static unsigned scans,backward,cases,avoided;
static size_t vfs_string_length(const char *p){++scans;size_t n=0;if(p)while(n<256&&p[n])++n;return n;}
''' + old + leaf + r'''
static void compare(const char *p){
 unsigned char a[80],b[80];memset(a,0x81,sizeof(a));memcpy(b,a,sizeof(b));
 scans=backward=0;previous_leaf(p,(char*)a+8);avoided+=backward;
 assert(scans==1);scans=0;vfs_set_leaf_name(p,(char*)b+8);
 assert(!scans&&!memcmp(a,b,sizeof(a)));++cases;
}
int main(void){
 char path[256];
 for(unsigned n=0;n<=256;++n){
  memset(path,'x',256);if(n<256)path[n]=0;compare(path);
  for(unsigned slash=0;slash<n;++slash){
   path[slash]='/';compare(path);path[slash]='x';
  }
  for(unsigned byte=1;byte<256;++byte){
   memset(path,byte,256);if(n<256)path[n]=0;compare(path);
  }
 }
 compare(0);compare("/");compare("/tmp/");compare("/tmp/file");
 long page=sysconf(_SC_PAGESIZE);assert(page>=256);
 char *memory=mmap(0,(size_t)page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 assert(memory!=MAP_FAILED&&!mprotect(memory+page,(size_t)page,PROT_NONE));
 for(unsigned n=1;n<=256;++n){
  char *edge=memory+page-n;memset(edge,'x',n);
  if(n<256)edge[n-1]=0;compare(edge);
  if(n>1){edge[0]='/';compare(edge);}
 }
 assert(!munmap(memory,(size_t)page*2));
 assert(avoided);printf("Stat leaf: %u exact full-output/canary cases; %u backward scan iterations removed; root/trailing slash/truncation/all bytes/null/protected bounds passed\n",cases,avoided);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-leaf-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-D_GNU_SOURCE', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-Wno-misleading-indentation', '-fsanitize=address,undefined',
                    '-fno-pie', '-no-pie', '-x', 'c', '-', '-o', binary],
                   input=code, text=True, check=True)
    subprocess.run([binary], check=True)
