"""Execute ordered link expansion, nofollow and bounded traversal."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/fs/vfs/core.c').read_text()
begin = source.index('kstatus_t vfs_resolve_path_kernel(')
end = source.index('kstatus_t vfs_stat_kernel(', begin)
program = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <string.h>
#include <kernel/base.h>
#include <uapi/file.h>
#define VFS_PATH_LIMIT 256
struct entry {const char *path;unsigned type;const char *target;};
static struct entry entries[] = {
 {"/tmp",OS_FILE_TYPE_DIRECTORY,0},
 {"/a",OS_FILE_TYPE_DIRECTORY,0},
 {"/a/deep",OS_FILE_TYPE_DIRECTORY,0},
 {"/a/file",OS_FILE_TYPE_REGULAR,0},
 {"/tmp/file",OS_FILE_TYPE_REGULAR,0},
 {"/tmp/alias",OS_FILE_TYPE_SYMLINK,"/a/deep"},
 {"/tmp/relative",OS_FILE_TYPE_SYMLINK,"alias"},
 {"/tmp/parent",OS_FILE_TYPE_SYMLINK,"../a"},
 {"/tmp/dangling",OS_FILE_TYPE_SYMLINK,"missing"},
 {"/tmp/loop",OS_FILE_TYPE_SYMLINK,"loop"},
 {"/tmp/cycle1",OS_FILE_TYPE_SYMLINK,"cycle2"},
 {"/tmp/cycle2",OS_FILE_TYPE_SYMLINK,"cycle1"},
 {"/tmp/root",OS_FILE_TYPE_SYMLINK,"/"},
 {"/tmp/file-slash",OS_FILE_TYPE_SYMLINK,"file/"},
 {"/tmp/long",OS_FILE_TYPE_SYMLINK,0},
};
static unsigned reads,stats;
static size_t vfs_string_length(const char *text) {return text?strnlen(text,256):0;}
static kstatus_t vfs_stat_kernel(const char *path,os_file_info_t *info) {
 ++stats;
 for(unsigned i=0;i<sizeof(entries)/sizeof(entries[0]);++i)
  if(!strcmp(path,entries[i].path)){info->type=entries[i].type;return K_OK;}
 return K_ENOENT;
}
static kstatus_t vfs_readlink_kernel(const char *path,char *out,size_t capacity,size_t *bytes) {
 ++reads;
 for(unsigned i=0;i<sizeof(entries)/sizeof(entries[0]);++i)
  if(!strcmp(path,entries[i].path)) {
   size_t length=strlen(entries[i].target);assert(length<=capacity);
   memcpy(out,entries[i].target,length);*bytes=length;return K_OK;
  }
 return K_ENOENT;
}
''' + source[begin:end] + r'''
int main(void) {
 char out[256];
#define RESOLVE(path,follow,missing) vfs_resolve_path_kernel(path,follow,missing,out)
 assert(RESOLVE("/tmp/alias/../file",true,false)==K_OK && !strcmp(out,"/a/file"));
 assert(RESOLVE("/tmp/relative/../file",true,false)==K_OK && !strcmp(out,"/a/file"));
 assert(RESOLVE("/tmp/parent/file",true,false)==K_OK && !strcmp(out,"/a/file"));
 assert(RESOLVE("/tmp/alias",false,false)==K_OK && !strcmp(out,"/tmp/alias"));
 assert(RESOLVE("/tmp/alias/",false,false)==K_OK && !strcmp(out,"/a/deep"));
 assert(RESOLVE("/tmp/file-slash",true,false)==K_ENOTDIR);
 assert(RESOLVE("//tmp///./file",true,false)==K_OK && !strcmp(out,"/tmp/file"));
 assert(RESOLVE("/tmp/file/..",true,false)==K_ENOTDIR);
 assert(RESOLVE("/tmp/file/.",true,false)==K_ENOTDIR);
 assert(RESOLVE("/absent/../tmp/file",true,false)==K_ENOENT);
 assert(RESOLVE("/tmp/dangling",false,false)==K_OK && !strcmp(out,"/tmp/dangling"));
 assert(RESOLVE("/tmp/dangling",true,false)==K_ENOENT);
 assert(RESOLVE("/tmp/dangling",true,true)==K_OK && !strcmp(out,"/tmp/missing"));
 assert(RESOLVE("/tmp/new",true,true)==K_OK && !strcmp(out,"/tmp/new"));
 assert(RESOLVE("/tmp/new/",true,true)==K_ENOENT);
 assert(RESOLVE("/tmp/absent/new",true,true)==K_ENOENT);
 assert(RESOLVE("/tmp/root",true,false)==K_OK && !strcmp(out,"/"));
 reads=0;assert(RESOLVE("/tmp/loop",true,false)==K_ELOOP && reads==40);
 reads=0;assert(RESOLVE("/tmp/cycle1",true,false)==K_ELOOP && reads==40);
 reads=0;assert(RESOLVE("/tmp/loop",false,false)==K_OK && !reads);
 char target[256];memset(target,'x',250);target[250]=0;
 entries[14].target=target;
 assert(RESOLVE("/tmp/long/suffix",true,false)==K_ENAMETOOLONG);
 assert(RESOLVE(0,true,false)==K_EINVAL);
 assert(RESOLVE("relative",true,false)==K_EINVAL);
 char too_long[300];memset(too_long,'x',299);too_long[0]='/';too_long[299]=0;
 assert(RESOLVE(too_long,true,false)==K_ENAMETOOLONG);
 assert(stats && !vfs_resolve_path_kernel("/",true,false,out) && !strcmp(out,"/"));
}
'''
with tempfile.TemporaryDirectory(prefix='vfs-link-resolve-') as directory:
    base = Path(directory)
    (base/'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I'+str(root/'include'), str(base/'test.c'), '-o', str(base/'test')],
                   check=True)
    subprocess.run([str(base/'test')], check=True)
print('VFS link resolution: ordered relative/absolute expansion and dotdot, '
      'nofollow, trailing separators, dangling/create paths, loops and bounds passed')
