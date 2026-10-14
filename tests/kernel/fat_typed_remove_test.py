"""Actual FAT deletion type guard and serialized wrapper, including raced type."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root/'kernel/fs/nativefs/fat32.c').read_text()
start = source.index('    if (directory >= 0 &&')
guard = source[start:source.index('    first_cluster =', start)]
start = source.index('BOOLEAN fat32_remove_path_typed(')
wrapper = source[start:source.index('static BOOLEAN fat32_restore_directory_slots(', start)]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
typedef bool BOOLEAN;
typedef char CHAR8;
typedef int OS_FAT32;
#define FAT32_DIRECTORY 16U
static bool locked;
static unsigned char entry[32];
static unsigned removed,flushes;
static bool fail_flush,fail_delete;
static void fat32_mutation_lock(OS_FAT32 *fs){assert(fs && !locked);locked=true;}
static void fat32_mutation_unlock(OS_FAT32 *fs){assert(fs && locked);locked=false;}
static bool fat32_writeback(OS_FAT32 *fs){assert(fs && locked);++flushes;return !fail_flush;}
static bool fat32_remove_path_locked(OS_FAT32 *fs,const CHAR8 *path,int directory){
 assert(fs && path && locked);
''' + guard + r'''
 if(fail_delete)return false;
 ++removed;return true;
}
''' + wrapper + r'''
int main(void){
 OS_FAT32 fs=1;
 for(int disk_directory=0;disk_directory<2;++disk_directory)
 for(int requested=-1;requested<=1;++requested){
  removed=flushes=0;entry[11]=disk_directory?FAT32_DIRECTORY:0;
  bool match=requested<0 || requested==disk_directory;
  assert(fat32_remove_path_typed(&fs,"victim",requested)==match);
  assert(!locked && removed==(unsigned)match && flushes==1+(unsigned)match);
 }
 removed=flushes=0;entry[11]=FAT32_DIRECTORY;
 assert(!fat32_remove_path_typed(&fs,"was-file-now-directory",0) && !removed && !locked);
 assert(fat32_remove_path(&fs,"legacy") && removed==1 && !locked);
 fail_flush=true;assert(!fat32_remove_path_typed(&fs,"victim",1) && removed==1 && !locked);
 fail_flush=false;fail_delete=true;
 assert(!fat32_remove_path_typed(&fs,"victim",1) && removed==1 && !locked);
 assert(!fat32_remove_path_typed(0,"victim",1) && !locked);
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)/'fat.c'
    path.write_text(code)
    binary = Path(directory)/'fat'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(path),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('FAT typed remove: real locked type gate, replacement-directory rejection, legacy behavior, flush/delete errors and balanced lock passed')
