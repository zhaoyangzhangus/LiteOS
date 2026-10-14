"""Actual page-load error branch must reject incomplete executable data."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/vfs/page_cache.c').read_text()
start = source.index('        if (status != K_OK || bytes != read_length) {')
end = source.index('\n    if (prefetch && bytes <= PAGE_SIZE)', start)
branch = source[start:end].rsplit('\n    }', 1)[0]
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define K_OK 0
#define K_EIO -5
static int freed_pages, freed_entries, unlocked;
static void page_free(void *p) { assert(p); ++freed_pages; }
static void kfree(void *p) { if(p) ++freed_entries; }
static void vfs_vnode_unlock(void *p) { assert(p); ++unlocked; }
static int run(int status, uint64_t bytes, uint64_t read_length, int prefetch) {
 int storage;
 void *page=&storage, *entry=&storage, *vnode=&storage;
 void *next_entry=prefetch ? &storage : 0;
''' + branch + r'''
 return K_OK;
}
int main(void) {
 const uint64_t lengths[]={1, 37, 4096, 8192};
 for(unsigned i=0;i<4;++i) for(int p=0;p<2;++p) {
  uint64_t len=lengths[i];
  for(int kind=0;kind<4;++kind) {
   freed_pages=freed_entries=unlocked=0;
   int status=kind==3 ? -12 : K_OK;
   uint64_t bytes=kind==0 ? len : kind==1 ? len-1 : len+1;
   int result=run(status, bytes, len, p);
   if(kind==0) assert(result==K_OK && !freed_pages && !unlocked);
   else assert(result==(kind==3 ? -12 : K_EIO) && freed_pages==1
               && freed_entries==1+p && unlocked==1);
  }
 }
 puts("File page integrity: exact partial EOF, short/oversized I/O, error propagation and cleanup passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-page-short-read-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-x', 'c', '-',
                    '-o', binary], input=code, text=True, check=True)
    subprocess.run([binary], check=True)
