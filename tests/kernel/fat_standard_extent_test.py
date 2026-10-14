"""Actual standard-FAT extent discovery stops on fragmentation, EOF and errors."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'kernel/fs/nativefs/fat_table.c').read_text()
begin=source.index('UINT32 fat32_extent_length(')
end=source.index('\n}',begin)+2
code=r'''
#include <stdint.h>
#include <assert.h>
typedef uint32_t UINT32;
typedef int BOOLEAN;
typedef struct { BOOLEAN Fat3Available; UINT32 ClusterCount; } OS_FAT32;
static unsigned calls, stop=100, failure=100;
static BOOLEAN fat32_cluster_valid(const OS_FAT32 *fs, UINT32 c) {
 return c>=2 && c<=fs->ClusterCount+1;
}
static BOOLEAN fat32_read_next_cluster(const OS_FAT32 *fs, UINT32 c, UINT32 *next) {
 (void)fs;++calls;if(c==failure)return 0;
 *next=c==stop?0:c+1;return 1;
}
static BOOLEAN fat32_read_fat_entry(const OS_FAT32 *fs, UINT32 table, UINT32 c, UINT32 *out) {
 (void)fs;(void)table;(void)c;*out=8;return 1;
}
''' +source[begin:end]+r'''
int main(void) {
 OS_FAT32 fs={.ClusterCount=100};
 assert(fat32_extent_length(&fs,2)==16 && calls==15);
 calls=0;stop=6;assert(fat32_extent_length(&fs,2)==5 && calls==5);
 stop=100;failure=4;assert(fat32_extent_length(&fs,2)==3);
 failure=2;assert(fat32_extent_length(&fs,2)==1);
 failure=1000;stop=1000;assert(fat32_extent_length(&fs,100)==2);
 assert(fat32_extent_length(&fs,101)==1);
 fs.Fat3Available=1;assert(fat32_extent_length(&fs,2)==8);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-fat-extent-') as directory:
 binary=str(Path(directory)/'test')
 subprocess.run(['cc','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                 '-fno-pie','-no-pie','-x','c','-','-o',binary],input=code,text=True,check=True)
 subprocess.run([binary],check=True)
print('Standard FAT: bounded contiguous prefix, EOF, read failure, volume boundary and third-table compatibility passed')
