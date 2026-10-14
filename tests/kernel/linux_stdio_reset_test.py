"""Actual stdio replacement: transfer all old pins, release outside owner locks."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/syscall/linux.c').read_text()
header = (root / 'include/kernel/linux_abi.h').read_text()
kinds = header[header.index('enum linux_fd_kind {'):header.index('\n};')+3]
put = source[source.index('static void linux_fd_entry_put(linux_fd_entry_t *entry) {'):
             source.index('static void linux_unix_channel_destroy(')]
reset = source[source.index('void linux_fd_table_reset_stdio('):
               source.index('/* Caller holds the source table lock')]
fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
typedef struct { unsigned refs; } owned_t;
typedef owned_t nv_frontend_file_t;
typedef owned_t linux_proc_file_t;
typedef owned_t window_server_window_t;
typedef struct { owned_t *file; void *private_data; uint32_t flags;
 uint8_t kind,reserved[3];owned_t *description; } linux_fd_entry_t;
typedef struct { bool held; } spinlock_t;
typedef struct { spinlock_t lock;linux_fd_entry_t entries[8]; } linux_fd_table_t;
static bool locked,owner_held,reenter;
static unsigned releases;
static linux_fd_table_t *active;
void linux_fd_table_reset_stdio(linux_fd_table_t *,linux_fd_entry_t [3]);
void linux_fd_table_release_stdio(linux_fd_entry_t [3]);
static void spinlock_lock(spinlock_t *lock) { assert(!locked && !lock->held);locked=lock->held=true; }
static void spinlock_unlock(spinlock_t *lock) { assert(locked && lock->held);locked=lock->held=false; }
static void assert_stdio(void) {
 for(unsigned i=0;i<3;++i) {
  linux_fd_entry_t *e=&active->entries[i];
  assert(!e->file && !e->private_data && !e->description);
  assert(e->kind==LINUX_FD_CONSOLE && e->flags==(i?1U:0U));
  for(unsigned j=0;j<3;++j)assert(!e->reserved[j]);
 }
}
static void release(void *opaque) {
 if(!opaque)return;
 assert(!locked && !owner_held);assert_stdio();
 owned_t *p=opaque;assert(p->refs);--p->refs;++releases;
 if(reenter) {
  reenter=false;linux_fd_entry_t nested[3];
  linux_fd_table_reset_stdio(active,nested);
  linux_fd_table_release_stdio(nested);
 }
}
static void linux_description_put_external(void *p) { release(p); }
static void linux_file_put(void *p) { release(p); }
static void nv_frontend_file_put(void *p) { release(p); }
static void linux_proc_file_put(void *p) { release(p); }
static void window_server_put(void *p) { release(p); }
static void object_put(void *p) { release(p); }
'''
checks = r'''
int main(void) {
 (void)linux_fd_entry_put;
 unsigned cases=0;
 for(unsigned base=0;base<12;++base)for(unsigned mask=0;mask<64;++mask)
 for(unsigned nested=0;nested<2;++nested) {
  linux_fd_table_t table={0};owned_t objects[3]={0},descriptions[3]={0};
  linux_fd_entry_t previous[3],saved[3];unsigned expected=0;
  for(unsigned i=0;i<3;++i) {
   linux_fd_entry_t *e=&table.entries[i];unsigned kind=(base+i)%12;
   e->kind=kind;e->flags=0xffffffff;e->reserved[0]=0xa5;
   if(mask & (1U<<(i*2))) { e->description=&descriptions[i];descriptions[i].refs=1;++expected; }
   if((mask & (2U<<(i*2))) && kind!=LINUX_FD_UNUSED && kind!=LINUX_FD_CONSOLE && kind!=LINUX_FD_NULL) {
    objects[i].refs=1;++expected;
    if(kind==LINUX_FD_FILE)e->file=&objects[i];else e->private_data=&objects[i];
   }
  }
  memset(table.entries+3,0xa5,sizeof(table.entries)-sizeof(saved));
  linux_fd_entry_t suffix[5];memcpy(suffix,table.entries+3,sizeof(suffix));
  memcpy(saved,table.entries,sizeof(saved));active=&table;releases=0;reenter=nested!=0;
  owner_held=true;linux_fd_table_reset_stdio(&table,previous);owner_held=false;
  assert(!locked && !releases);assert_stdio();
  assert(!memcmp(previous,saved,sizeof(saved)));
  assert(!memcmp(suffix,table.entries+3,sizeof(suffix)));
  linux_fd_table_release_stdio(previous);
  assert(releases==expected);
  for(unsigned i=0;i<3;++i)assert(!objects[i].refs && !descriptions[i].refs);
  linux_fd_table_release_stdio(previous);assert(releases==expected);
  linux_fd_entry_t again[3];linux_fd_table_reset_stdio(&table,again);
  linux_fd_table_release_stdio(again);assert(releases==expected);
  linux_fd_entry_t untouched[3];memset(untouched,0xa5,sizeof(untouched));
  linux_fd_entry_t copy[3];memcpy(copy,untouched,sizeof(copy));
  linux_fd_table_reset_stdio(NULL,untouched);assert(!memcmp(copy,untouched,sizeof(copy)));
  linux_fd_table_reset_stdio(&table,NULL);linux_fd_table_release_stdio(NULL);
  assert(!locked && !owner_held);++cases;
 }
 linux_fd_table_t aliases={0};owned_t shared_file={3},shared_description={3};
 for(unsigned i=0;i<3;++i)aliases.entries[i]=(linux_fd_entry_t){
  .file=&shared_file,.kind=LINUX_FD_FILE,.description=&shared_description};
 linux_fd_entry_t old_aliases[3];active=&aliases;releases=0;reenter=true;
 owner_held=true;linux_fd_table_reset_stdio(&aliases,old_aliases);owner_held=false;
 assert(shared_file.refs==3 && shared_description.refs==3 && !releases);
 linux_fd_table_release_stdio(old_aliases);
 assert(!shared_file.refs && !shared_description.refs && releases==6);
 printf("%u mixed stdio layouts plus shared aliases: full detach, exact pins, lock-free/reentrant release, untouched suffix and repeated/null cleanup passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-stdio-reset-') as directory:
    path = Path(directory)
    def run(actual):
        (path / 'test.c').write_text(kinds + fixture + put + actual + checks)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        str(path / 'test.c'), '-o', str(path / 'test')], check=True)
        return subprocess.run([str(path / 'test')], capture_output=True, text=True)
    result = run(reset)
    assert result.returncode == 0, result.stdout + result.stderr
    print(result.stdout.strip())
    negative = reset.replace('previous[descriptor] = table->entries[descriptor];',
                             'previous[descriptor] = (linux_fd_entry_t){0};')
    result = run(negative)
    assert result.returncode != 0 and 'memcmp(previous' in result.stderr,result.stderr
    print('Negative control rejected: overwritten old entries lose ownership')
    negative = reset.replace('linux_fd_entry_put(&previous[descriptor]);',
                             'previous[descriptor] = (linux_fd_entry_t){0};')
    result = run(negative)
    assert result.returncode != 0 and 'releases==expected' in result.stderr,result.stderr
    print('Negative control rejected: detached references are leaked')
