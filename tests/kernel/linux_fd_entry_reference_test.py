"""Actual shared descriptor pin/unpin, every kind and nullable pointer tuple."""
from pathlib import Path
import resource
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/syscall/linux.c').read_text()
header=(root/'include/kernel/linux_abi.h').read_text()
enum=header[header.index('enum linux_fd_kind {'):header.index('\n};')+3]
get=source[source.index('static void linux_fd_entry_get('):source.index('void linux_fd_table_init(')]
put=source[source.index('static void linux_fd_entry_put(linux_fd_entry_t *entry) {'):
           source.index('static void linux_unix_channel_destroy(')]
clone=source[source.index('kstatus_t linux_fd_table_clone('):source.index('void linux_fd_table_close_on_exec(')]
lookup=source[source.index('static kstatus_t linux_fd_lookup('):source.index('static int64_t linux_fd_install_description(')]
assert 'linux_fd_entry_get(destination_entry);' in clone
assert 'linux_fd_entry_get(entry);' in lookup
code=r'''
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
typedef struct {atomic_uint refs;} owned_t;
typedef owned_t file_t;
typedef owned_t nv_frontend_file_t;
typedef owned_t linux_proc_file_t;
typedef owned_t window_server_window_t;
typedef struct {file_t *file;void *private_data;uint32_t flags;uint8_t kind;owned_t *description;} linux_fd_entry_t;
static _Thread_local bool locked;
static atomic_uint generic_gets,nvidia_gets,proc_gets,description_gets;
static void retain(void *p) {if(p){assert(locked);assert(atomic_fetch_add(&((owned_t*)p)->refs,1));}}
static void release(void *p) {if(p){assert(!locked);assert(atomic_fetch_sub(&((owned_t*)p)->refs,1)>1);}}
static void object_get(void *p){if(p)++generic_gets;retain(p);}
static void object_put(void *p){release(p);}
static void nv_frontend_file_get(void *p){if(p)++nvidia_gets;retain(p);}
static void linux_proc_file_get(void *p){if(p)++proc_gets;retain(p);}
static void linux_description_get_external(void *p){if(p)++description_gets;retain(p);}
static void nv_frontend_file_put(void *p){release(p);}
static void linux_proc_file_put(void *p){release(p);}
static void linux_description_put_external(void *p){release(p);}
static void linux_file_put(void *p){release(p);}
static void window_server_put(void *p){release(p);}
''' + enum + '\n' + get + '\n' + put + r'''
static owned_t objects[3];
static linux_fd_entry_t shared;
static pthread_mutex_t source_lock=PTHREAD_MUTEX_INITIALIZER;
static void *worker(void *opaque) {
 (void)opaque;
 for(unsigned i=0;i<10000;++i) {
  assert(!pthread_mutex_lock(&source_lock));locked=true;
  linux_fd_entry_t local=shared;linux_fd_entry_get(&local);
  locked=false;assert(!pthread_mutex_unlock(&source_lock));
  assert(atomic_load(&objects[1].refs)>1 && atomic_load(&objects[2].refs)>1);
  linux_fd_entry_put(&local);
 }
 return 0;
}
int main(void) {
 for(unsigned i=0;i<3;++i)atomic_init(&objects[i].refs,1);
 for(unsigned kind=0;kind<=LINUX_FD_NULL;++kind)for(unsigned mask=0;mask<8;++mask) {
  linux_fd_entry_t entry={0};entry.kind=kind;entry.flags=0x87654321;
  if(mask&1) {entry.file=&objects[0];}
  if(mask&2) {entry.private_data=&objects[1];}
  if(mask&4) {entry.description=&objects[2];}
  linux_fd_entry_t before=entry;
  atomic_store(&generic_gets,0);atomic_store(&proc_gets,0);
  atomic_store(&nvidia_gets,0);atomic_store(&description_gets,0);
  locked=true;linux_fd_entry_get(&entry);locked=false;
  bool file=kind==LINUX_FD_FILE && (mask&1);
  bool special=(kind==LINUX_FD_PROC || kind==LINUX_FD_NVIDIA) && (mask&2);
  bool generic=kind>=LINUX_FD_WINDOW && kind<=LINUX_FD_UNIX_SOCKET && (mask&2);
  assert(!memcmp(&entry,&before,sizeof(entry)));
  assert(atomic_load(&objects[0].refs)==1U+file);
  assert(atomic_load(&objects[1].refs)==1U+(special||generic));
  assert(atomic_load(&objects[2].refs)==1U+!!(mask&4));
  assert(atomic_load(&generic_gets)==(unsigned)file+(unsigned)generic);
  assert(atomic_load(&proc_gets)==(unsigned)(kind==LINUX_FD_PROC && (mask&2)));
  assert(atomic_load(&nvidia_gets)==(unsigned)(kind==LINUX_FD_NVIDIA && (mask&2)));
  assert(atomic_load(&description_gets)==!!(mask&4));
  linux_fd_entry_put(&entry);
  assert(!entry.file && !entry.private_data && !entry.description);
  assert(entry.kind==before.kind && entry.flags==before.flags);
  for(unsigned i=0;i<3;++i)assert(atomic_load(&objects[i].refs)==1);
 }
 linux_fd_entry_put(0);
 shared=(linux_fd_entry_t){&objects[0],&objects[1],19,LINUX_FD_UNIX_SOCKET,&objects[2]};
 pthread_t workers[4];for(unsigned i=0;i<4;++i)assert(!pthread_create(&workers[i],0,worker,0));
 for(unsigned i=0;i<4;++i)assert(!pthread_join(workers[i],0));
 for(unsigned i=0;i<3;++i)assert(atomic_load(&objects[i].refs)==1);
 puts("FD references: 96 type/pointer combinations, unchanged source entries, correct object/description dispatch and 40000 concurrent pin/release cycles passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-fd-reference-') as directory:
    binary=str(Path(directory)/'test')
    command=['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
             '-pthread','-fsanitize=address,undefined',
             '-fno-pie','-no-pie','-x','c','-','-o',binary]
    subprocess.run(command,input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
    negative=code.replace('    linux_description_get_external(entry->description);',
                          '    if (false) linux_description_get_external(entry->description);')
    assert negative!=code
    subprocess.run(command,input=negative,text=True,check=True)
    result=subprocess.run([binary],capture_output=True,text=True,timeout=30,
                          preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
    assert result.returncode and 'Assertion' in result.stderr
    print('Negative control rejected: concrete object pinned without its description')
