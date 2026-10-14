"""Production root/secondary mount publication and allocation rollback."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/init/filesystem_root.c').read_text()
code=source[source.index('static BOOLEAN filesystem_mount_candidate('):source.index('static BOOLEAN filesystem_mount_selected_root(')]
program=r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned UINT32;typedef int BOOLEAN;typedef char CHAR8;
#define FILESYSTEM_VOLUME_LIMIT 32U
#define FILESYSTEM_PATH_LIMIT 256U
#define K_OK 0
typedef struct {UINT32 mounted_volume;} filesystem_candidate_t;
typedef struct {int a;} filesystem_slice_t;
typedef struct {int a;} OS_BLOCK_DEVICE;
typedef struct {int initialized;} OS_FAT32;
typedef struct {filesystem_slice_t slice;OS_BLOCK_DEVICE block_device;OS_FAT32 filesystem;} filesystem_volume_t;
static filesystem_volume_t* g_volumes[FILESYSTEM_VOLUME_LIMIT];
static filesystem_candidate_t g_candidates[40];
static UINT32 g_candidate_count=40,g_root_candidate=UINT32_MAX;
static BOOLEAN g_root_mounted;
static unsigned allocations,frees,destroys,mounts;
static int oom,init_ok=1,required_ok=1,path_ok=1,vfs_ok=1;
static OS_FAT32* mounted[40];
static void* kzalloc(size_t size,int flags){(void)flags;if(oom)return 0;void* p=calloc(1,size);assert(p);++allocations;return p;}
static void kfree(void* p){assert(p);++frees;free(p);}
static void filesystem_zero(void* p,size_t n){memset(p,0,n);}
static void filesystem_log_candidate(const char* s,filesystem_candidate_t* c){(void)s;(void)c;}
static void filesystem_prepare_slice(filesystem_candidate_t* c,filesystem_slice_t* s){(void)c;(void)s;}
static void filesystem_prepare_block_device(filesystem_slice_t* s,OS_BLOCK_DEVICE* b){(void)s;(void)b;}
static int fat32_init(OS_FAT32* f,OS_BLOCK_DEVICE* b){(void)b;f->initialized=init_ok;return init_ok;}
static int fat32_destroy(OS_FAT32* f){assert(f->initialized);++destroys;return 1;}
static int filesystem_has_required_files(OS_FAT32* f){assert(f->initialized);return required_ok;}
static void kernel_serial_write(const char* s){(void)s;}
static void realtest_fat_lost(const char* s){(void)s;}
static void realtest_fat_ready(OS_FAT32* f){assert(f->initialized);}
static int filesystem_make_mount_path(filesystem_candidate_t* c,char* p){(void)c;p[0]=0;return path_ok;}
static int vfs_mount_fat32(const char* p,OS_FAT32* f){(void)p;if(!vfs_ok)return -1;mounted[mounts++]=f;return 0;}
'''+code+r'''
int main(void){
 for(unsigned i=0;i<40;i++)g_candidates[i].mounted_volume=UINT32_MAX;
 assert(!filesystem_mount_candidate(40,1) && !allocations);
 oom=1;assert(!filesystem_mount_candidate(0,1) && !allocations);oom=0;
 init_ok=0;assert(!filesystem_mount_candidate(0,1) && allocations==1 && frees==1 && !destroys);init_ok=1;
 required_ok=0;assert(!filesystem_mount_candidate(0,1) && frees==2 && destroys==1);required_ok=1;
 vfs_ok=0;assert(!filesystem_mount_candidate(0,1) && frees==3 && destroys==2);vfs_ok=1;
 assert(g_candidates[0].mounted_volume==UINT32_MAX && !g_volumes[0]);
 assert(filesystem_mount_candidate(0,1) && g_root_mounted && g_root_candidate==0);
 assert(g_candidates[0].mounted_volume==0 && mounted[0]==&g_volumes[0]->filesystem);
 unsigned before=allocations;assert(filesystem_mount_candidate(0,1) && allocations==before);
 assert(!filesystem_mount_candidate(0,0) && allocations==before);
 path_ok=0;assert(!filesystem_mount_candidate(1,0) && destroys==3 && !g_volumes[1]);path_ok=1;
 vfs_ok=0;assert(!filesystem_mount_candidate(1,0) && destroys==4 && !g_volumes[1]);vfs_ok=1;
 for(unsigned i=1;i<32;i++){
   assert(filesystem_mount_candidate(i,0));assert(g_candidates[i].mounted_volume==i);
   assert(mounted[i]==&g_volumes[i]->filesystem && mounted[0]==&g_volumes[0]->filesystem);
 }
 before=allocations;assert(!filesystem_mount_candidate(32,0) && allocations==before);
 for(unsigned i=0;i<32;i++){kfree(g_volumes[i]);}
 assert(allocations==frees);
}
'''
with tempfile.TemporaryDirectory() as directory:
    path=Path(directory);(path/'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie',str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('Lazy volumes: OOM/init/required-files/VFS/path rollback, retry, duplicate mounts, stable pointers and 32-volume limit passed')
