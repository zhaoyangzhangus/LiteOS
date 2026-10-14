#!/usr/bin/env python3
"""Exercise the production adaptive cache index with allocation failures."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
header = (root / 'kernel/fs/vfs/internal.h').read_text()
types = header[header.index('typedef struct vfs_cached_page {'):
               header.index('static inline void vfs_backend_lock(')]
source = (root / 'kernel/fs/vfs/page_cache.c').read_text()
helpers = source[source.index('static uint32_t vfs_page_cache_bucket('):
                 source.index('static void vfs_vm_mapping_retain(')]
helpers = helpers.replace('if (entry->index == page_index)',
                          '++visits; if (entry->index == page_index)')
code = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
typedef void page_t;
static unsigned allocations, failures, live, visits;
static void *kzalloc(size_t n, int flags) {
 (void)flags; ++allocations;
 if (failures) { --failures; return NULL; }
 void *p=calloc(1,n); assert(p); ++live; return p;
}
static void kfree(void *p) { if(p) { assert(live); --live; free(p); } }
''' + types + helpers + r'''
static void verify(vfs_page_cache_t *cache, vfs_cached_page_t *entries, unsigned n) {
 unsigned list_count=0;
 for(vfs_cached_page_t *e=cache->head;e;e=e->next) {
  assert(e>=entries && e<entries+n); assert(++list_count<=n);
 }
 assert(list_count==n && cache->entry_count==n);
 for(unsigned i=0;i<n;++i) assert(vfs_page_cache_lookup(cache,i)==entries+i);
 assert(vfs_page_cache_lookup(cache,UINT64_MAX)==NULL);
}
int main(void) {
 vfs_page_cache_t cache={0};
 vfs_cached_page_t *entries=calloc(65536,sizeof(*entries)); assert(entries);
 assert(!vfs_page_cache_lookup(NULL,0));
 for(unsigned i=0;i<1023;++i) {
  entries[i].index=i; vfs_page_cache_insert(&cache,entries+i);
  vfs_page_cache_grow(&cache);
 }
 assert(!allocations && !cache.expanded_buckets);
 failures=2;
 for(unsigned i=1023;i<1280;++i) {
  entries[i].index=i; vfs_page_cache_insert(&cache,entries+i);
  vfs_page_cache_grow(&cache);
 }
 assert(allocations==2 && !live && cache.growth_retry_at==1536);
 verify(&cache,entries,1280);
 /* Demand + read-ahead can cross a threshold by two; odd counts must grow. */
 for(unsigned i=1280;i<65536;i+=2) {
  entries[i].index=i; vfs_page_cache_insert(&cache,entries+i);
  if(i+1<65536) {
   entries[i+1].index=i+1; vfs_page_cache_insert(&cache,entries+i+1);
  }
  vfs_page_cache_grow(&cache);
 }
 verify(&cache,entries,65536);
 assert(cache.bucket_count==16384 && live==1);
 visits=0; assert(vfs_page_cache_lookup(&cache,12345)==entries+12345);
 assert(visits<=4); printf("65536 cached pages: lookup visits=%u, buckets=%u\n",visits,cache.bucket_count);
 unsigned before=allocations; vfs_page_cache_grow(&cache); assert(allocations==before);
 kfree(cache.expanded_buckets); free(entries); assert(!live);
 /* Crossing the very first threshold with paired insertions from an odd base. */
 cache=(vfs_page_cache_t){0}; entries=calloc(1025,sizeof(*entries)); assert(entries);
 for(unsigned i=0;i<1025;++i) { entries[i].index=i; vfs_page_cache_insert(&cache,entries+i); }
 vfs_page_cache_grow(&cache); assert(cache.bucket_count==512);
 verify(&cache,entries,1025); kfree(cache.expanded_buckets); free(entries);
 assert(!live); puts("Growth, OOM retry, list ownership, paired insertion and cap passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-cache-growth-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-x', 'c', '-', '-o', binary],
                   input=code, text=True, check=True)
    subprocess.run([binary], check=True)
