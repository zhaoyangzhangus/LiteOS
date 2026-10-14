#!/usr/bin/env python3
"""Run the real demand-page allocation branch with failed optional prefetch."""
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/vfs/page_cache.c').read_text()
start = source.index('    page = page_alloc(prefetch ? 1U : 0U, 0);')
end = source.index('\n    if (page == 0) {', start)
branch = source[start:end]
code = r'''
#include <assert.h>
#include <stdio.h>
#define PAGE_ALLOC_ZERO 1U
static int page_storage, entry_storage, freed, calls, demand_ok, prefetch_ok;
static void *page_alloc(unsigned order, unsigned flags) {
 assert(flags == 0); ++calls;
 return (order ? prefetch_ok : demand_ok) ? &page_storage : 0;
}
static void kfree(void *p) { assert(p==&entry_storage); ++freed; }
static void run(int want_prefetch) {
 int prefetch=want_prefetch;
 void *next_entry=want_prefetch ? &entry_storage : 0;
 void *page;
''' + branch + r'''
 if(want_prefetch && !prefetch_ok) {
  assert(!prefetch && !next_entry && freed==1 && calls==2);
 } else { assert(calls==1 && !freed); }
 assert((page!=0)==(want_prefetch && prefetch_ok ? 1 : demand_ok));
}
int main(void) {
 for(int a=0;a<2;++a) for(int b=0;b<2;++b) for(int p=0;p<2;++p) {
  freed=calls=0; demand_ok=a; prefetch_ok=b;run(p);
 }
 puts("file page: optional prefetch, fragmented demand fallback and true exhaustion passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-file-prefetch-') as directory:
    binary = str(pathlib.Path(directory) / 'test')
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-x', 'c', '-', '-o', binary],
                   input=code, text=True, check=True)
    subprocess.run([binary], check=True)
