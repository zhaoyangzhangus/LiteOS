"""Production FAT fsync follows the open file rather than the namespace."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/fs/vfs/backend.c').read_text()
function = source[source.index('kstatus_t vfs_fat32_backend_fsync('):
                  source.index('kstatus_t vfs_fat32_backend_truncate(')]
code = r'''
#include <assert.h>
#include <stdbool.h>
#define VFS_PATH_LIMIT 256
#define K_OK 0
#define K_EIO -5
typedef int kstatus_t;
typedef struct { unsigned id; } OS_FAT32;
typedef struct { bool Used; OS_FAT32 *FileSystem; } OS_FAT32_FILE;
typedef struct { bool detached; OS_FAT32_FILE *fat_file; char path[256]; } vfs_backend_node_t;
typedef struct { OS_FAT32 *filesystem; } vfs_fat32_mount_t;
static OS_FAT32 opened={1},replacement={2};
static vfs_fat32_mount_t mount={&replacement};
static unsigned lookups,syncs;
static bool lookup_ok=true,sync_ok=true;
static OS_FAT32 *expected;
static vfs_fat32_mount_t *vfs_fat32_mount_for_path(const char *path,char *relative) {
    assert(path && relative);++lookups;return lookup_ok?&mount:0;
}
static bool fat32_sync(OS_FAT32 *fs) {
    assert(fs==expected);++syncs;return sync_ok;
}
''' + function + r'''
int main(void) {
    OS_FAT32_FILE file={true,&opened};
    vfs_backend_node_t backend={.fat_file=&file};expected=&opened;
    for(unsigned i=0;i<1000;++i)assert(vfs_fat32_backend_fsync(&backend)==K_OK);
    assert(syncs==1000 && !lookups); // namespace now resolves to another FS
    lookup_ok=false;
    assert(vfs_fat32_backend_fsync(&backend)==K_OK && !lookups);
    sync_ok=false;
    assert(vfs_fat32_backend_fsync(&backend)==K_EIO && syncs==1002);
    file.Used=false;
    assert(vfs_fat32_backend_fsync(&backend)==K_EIO && syncs==1002 && !lookups);
    file.Used=true;file.FileSystem=0;
    assert(vfs_fat32_backend_fsync(&backend)==K_EIO && !lookups);
    backend.detached=true;
    assert(vfs_fat32_backend_fsync(&backend)==K_OK && syncs==1002 && !lookups);
    backend.detached=false;backend.fat_file=0;
    assert(vfs_fat32_backend_fsync(&backend)==K_EIO && lookups==1);
    lookup_ok=true;sync_ok=true;expected=&replacement;
    assert(vfs_fat32_backend_fsync(&backend)==K_OK && lookups==2 && syncs==1003);
    sync_ok=false;
    assert(vfs_fat32_backend_fsync(&backend)==K_EIO && lookups==3);
    assert(vfs_fat32_backend_fsync(0)==K_EIO && lookups==3);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-vfs-fsync-') as directory:
    path=Path(directory)
    (path/'test.c').write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('FAT fsync: 1000 open-file syncs without mount scans; filesystem identity, detached/invalid handles and fallback errors passed')
