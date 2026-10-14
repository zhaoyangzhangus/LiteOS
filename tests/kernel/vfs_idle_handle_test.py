"""Actual idle FAT handle reclamation: identity, refs, locking and races."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/vfs/core.c').read_text()
start = source.index('static bool vfs_release_idle_fat_handle(')
helper = source[start:source.index('static kstatus_t vfs_create_fat32_vnode(', start)]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
typedef struct {unsigned id;} OS_FAT32;
typedef struct {OS_FAT32 *FileSystem;} fat_file_t;
typedef struct {fat_file_t *fat_file;bool detached;} vfs_backend_node_t;
typedef struct {struct {struct {atomic_uint value;} refs;} object;void *fs_private;} vnode_t;
static vnode_t *g_vfs_nodes[8];static uint32_t g_vfs_node_capacity=8;
static vfs_backend_node_t b[8];static vnode_t v[8];static fat_file_t files[8];
static OS_FAT32 fs={1},other={2};
static unsigned ns_lock,io_lock,gets,drops,closes;
static bool race_open,close_fail;
static void vfs_lock(void){assert(!ns_lock&&!io_lock);ns_lock=1;}
static void vfs_unlock(void){assert(ns_lock&&!io_lock);ns_lock=0;}
static void object_get(vnode_t *n){assert(ns_lock);atomic_fetch_add(&n->object.refs.value,1);++gets;}
static void object_put(vnode_t *n){assert(!ns_lock&&!io_lock);assert(atomic_fetch_sub(&n->object.refs.value,1)>1);++drops;}
static void vfs_backend_lock(vfs_backend_node_t *n){
 assert(!ns_lock&&!io_lock);io_lock=1;
 if(race_open&&n==&b[0]){atomic_fetch_add(&v[0].object.refs.value,1);race_open=false;}
}
static void vfs_backend_unlock(vfs_backend_node_t *n){(void)n;assert(io_lock&&!ns_lock);io_lock=0;}
static bool fat32_close(fat_file_t *f){assert(io_lock&&!ns_lock&&f->FileSystem==&fs);++closes;return !close_fail;}
''' + helper + r'''
static void reset(void){
 assert(!ns_lock&&!io_lock);gets=drops=closes=0;race_open=close_fail=false;
 for(unsigned i=0;i<8;++i){files[i].FileSystem=&fs;b[i]=(vfs_backend_node_t){&files[i],false};
  v[i].fs_private=&b[i];atomic_init(&v[i].object.refs.value,1);g_vfs_nodes[i]=&v[i];}
}
int main(void){
 reset();assert(vfs_release_idle_fat_handle(&fs));
 assert(!b[0].fat_file&&closes==1&&gets==1&&drops==1);
 for(unsigned i=1;i<8;++i)assert(b[i].fat_file==&files[i]);
 reset();for(unsigned i=0;i<8;++i)atomic_store(&v[i].object.refs.value,2);
 assert(!vfs_release_idle_fat_handle(&fs)&&!gets&&!closes&&!drops);
 reset();files[0].FileSystem=&other;b[1].detached=true;b[2].fat_file=0;g_vfs_nodes[3]=0;
 assert(vfs_release_idle_fat_handle(&fs)&&!b[4].fat_file);
 assert(b[0].fat_file&&b[1].fat_file&&gets==4&&drops==4&&closes==1);
 reset();race_open=true;assert(vfs_release_idle_fat_handle(&fs));
 assert(b[0].fat_file&&!b[1].fat_file&&atomic_load(&v[0].object.refs.value)==2);
 assert(gets==2&&drops==2&&closes==1);
 reset();close_fail=true;assert(!vfs_release_idle_fat_handle(&fs));
 assert(closes==8&&gets==8&&drops==8);
 for(unsigned i=0;i<8;++i)assert(b[i].fat_file&&atomic_load(&v[i].object.refs.value)==1);
 g_vfs_node_capacity=0;assert(!vfs_release_idle_fat_handle(&fs));
 puts("Idle FAT reclamation: one handle only, active/mapped refs protected, filesystem/detached identity, reopen race, close failure and balanced pins outside namespace lock passed");
}
'''
with tempfile.TemporaryDirectory(prefix='vfs-idle-handle-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-x', 'c', '-', '-o', binary], input=code, text=True, check=True)
    subprocess.run([binary], check=True)
