#!/usr/bin/env python3
"""Exercise the production refill fallback with a fragmented buddy allocator."""
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
source = (root / 'kernel/mm/phys/percpu_page.c').read_text()
start = source.index('        lock_pages();', source.index('static PAGE_NOINLINE page_t *pool_alloc_slow'))
end = source.index('\n        base_pfn =', start)
refill = source[start:end]
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
typedef struct { unsigned refs; } page_t;
#define PAGE_POOL_GROUP_SHIFT 6U
static page_t page;
static unsigned locked, calls, available, requested;
static void lock_pages(void) { assert(!locked); locked=1; }
static void unlock_pages(void) { assert(locked); locked=0; }
static page_t *take_from_zone_locked(unsigned zone, unsigned order) {
 assert(locked && zone==1); ++calls;
 if(order==requested && available) { page.refs=1; return &page; }
 return 0;
}
static page_t *refill(unsigned zone, uint8_t order) {
''' + refill + r'''
 return block;
}
int main(void) {
 for(requested=0;requested<4;++requested) {
  calls=0;available=1;
  assert(refill(1,requested)==&page && page.refs==1 && calls==2 && !locked);
  calls=0;available=0;
  assert(refill(1,requested)==0 && calls==2 && !locked);
 }
 puts("page-pool: fragmented refill and exhaustion, orders 0..3 OK");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-pool-fragmentation-') as directory:
    binary = str(pathlib.Path(directory) / 'test')
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-x', 'c', '-', '-o', binary],
                   input=code, text=True, check=True)
    subprocess.run([binary], check=True)
