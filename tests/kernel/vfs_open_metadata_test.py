"""Actual FAT vnode factory prefix reuses caller metadata, not stale create info."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/vfs/core.c').read_text()
start = source.index('static kstatus_t vfs_create_fat32_vnode(')
prefix = source[start:source.index('    backend = (vfs_backend_node_t *)kzalloc', start)]
assert 'mounted_exists ? &mounted_info : 0, &vnode' in source
assert 'path, (flags & VFS_OPEN_WRITE) == 0U, 0, &vnode' in source
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define VFS_PATH_LIMIT 256U
typedef int kstatus_t;
enum {K_OK,K_ENOENT,K_ENOMEM};
enum {OS_FILE_TYPE_UNKNOWN,OS_FILE_TYPE_DIRECTORY,OS_FILE_TYPE_REGULAR};
typedef struct {unsigned type,mode;uint64_t size;} os_file_info_t;
typedef struct {unsigned unused;} vfs_backend_node_t;
typedef struct {unsigned refs;} vnode_t;
typedef struct {void *filesystem;} vfs_fat32_mount_t;
static vfs_fat32_mount_t mount={(void*)1};static vnode_t node={1};
static unsigned stats,lock;static bool stat_ok=true,parent=true,hit=true;
static vfs_fat32_mount_t *vfs_fat32_mount_for_path(const char *p,char *r){
 assert(p);strcpy(r,!strcmp(p,"/")?"":"/entry");return &mount;
}
static bool fat32_stat_path(void *fs,const char *p,os_file_info_t *i){
 assert(fs==mount.filesystem&&p);++stats;*i=(os_file_info_t){OS_FILE_TYPE_REGULAR,0644,42};return stat_ok;
}
static bool vfs_mount_parent_exists(const char *p,size_t n){assert(p&&n);return parent;}
static size_t vfs_string_length(const char *p){return strlen(p);}
static void vfs_initialize(void){}
static void vfs_lock(void){assert(!lock);lock=1;}
static void vfs_unlock(void){assert(lock);lock=0;}
static vnode_t *vfs_find_fat_node_locked(const char *p){assert(lock&&p);return hit?&node:0;}
static void object_get(vnode_t *n){assert(lock&&n==&node);++n->refs;}
''' + prefix + r'''
    (void)backend;(void)vnode;(void)keep_fat_file;
    return K_ENOMEM; /* Boundary: normal allocation follows the tested prefix. */
}
int main(void){
 os_file_info_t known={OS_FILE_TYPE_REGULAR,0600,100};vnode_t *out=0;
 for(unsigned i=0;i<1000;++i){
  assert(vfs_create_fat32_vnode("/entry",i%2,&known,&out)==K_OK);
  assert(out==&node&&!stats&&!lock);--node.refs;
 }
 assert(vfs_create_fat32_vnode("/entry",true,0,&out)==K_OK&&stats==1);--node.refs;
 assert(vfs_create_fat32_vnode("/",true,0,&out)==K_OK&&stats==1);--node.refs;
 stat_ok=false;parent=false;
 assert(vfs_create_fat32_vnode("/entry",true,0,&out)==K_ENOENT&&stats==2&&!lock);
 assert(vfs_create_fat32_vnode("/entry",true,&known,&out)==K_OK&&stats==2);--node.refs;
 hit=false;stat_ok=true;
 assert(vfs_create_fat32_vnode("/entry",false,0,&out)==K_ENOMEM&&stats==3&&!lock);
 assert(vfs_create_fat32_vnode("/entry",false,&known,&out)==K_ENOMEM&&stats==3&&!lock);
 assert(vfs_create_fat32_vnode("/entry",true,&known,0)==K_ENOENT&&stats==3);
 assert(node.refs==1);
 puts("FAT open metadata: 1000 cached opens avoid second stat; create/missing/root/allocation boundary and balanced refs passed");
}
'''
with tempfile.TemporaryDirectory(prefix='vfs-open-metadata-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-x', 'c', '-', '-o', binary], input=code, text=True, check=True)
    subprocess.run([binary], check=True)
