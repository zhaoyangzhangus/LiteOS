"""Production boot/cached sector reads preserve retry, range and diagnostic paths."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'kernel/fs/nativefs/fat_table.c').read_text()
code=source[source.index('static BOOLEAN fat32_read_sector_cached('):source.index('BOOLEAN fat32_write_sector(')]
program=r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
typedef int BOOLEAN;typedef uint64_t UINT64;typedef unsigned UINT32;typedef unsigned char UINT8;
typedef struct {UINT64 BlockCount;} OS_BLOCK_DEVICE;
typedef struct {int Initialized;} OS_BLOCK_CACHE;
typedef struct {OS_BLOCK_DEVICE* Device;OS_BLOCK_CACHE Cache;} OS_FAT32;
static unsigned direct,cached,diagnostics,failures;
static const char* tag;static UINT64 diag_lba,diag_count,diag_cache;
static void fat32_diag_failure(const char* t,UINT64 a,UINT64 b,UINT64 c){++diagnostics;tag=t;diag_lba=a;diag_count=b;diag_cache=c;}
static int read_attempt(UINT8* b){if(failures){--failures;return 0;}b[0]=42;return 1;}
static BOOLEAN block_read(const OS_BLOCK_DEVICE* d,UINT64 lba,UINT32 n,UINT8* b){assert(d && lba<d->BlockCount && n==1);++direct;return read_attempt(b);}
static BOOLEAN block_cache_read(OS_BLOCK_CACHE* c,UINT64 lba,UINT8* b){assert(c && c->Initialized);(void)lba;++cached;return read_attempt(b);}
'''+code+r'''
int main(void){
 UINT8 b[4096]={0};OS_BLOCK_DEVICE d={4};OS_FAT32 fs={.Device=&d};
 assert(fat32_read_boot_sector(&d,b) && b[0]==42 && direct==1 && cached==0);
 failures=2;assert(fat32_read_boot_sector(&d,b) && direct==4 && !diagnostics);
 failures=3;assert(!fat32_read_boot_sector(&d,b) && direct==7 && diagnostics==1);
 assert(!strcmp(tag,"FAT_SECTOR_IO_FAIL") && diag_lba==0 && diag_count==4 && diag_cache==0);
 assert(!fat32_read_boot_sector(0,b));assert(!fat32_read_boot_sector(&d,0));
 d.BlockCount=0;assert(!fat32_read_boot_sector(&d,b) && direct==7);d.BlockCount=4;
 assert(!fat32_read_sector(0,0,b));assert(!fat32_read_sector(&fs,4,b));
 assert(fat32_read_sector(&fs,3,b) && direct==8);
 fs.Cache.Initialized=1;failures=2;assert(fat32_read_sector(&fs,3,b) && cached==3 && direct==8);
 failures=3;assert(!fat32_read_sector(&fs,3,b) && cached==6 && diag_cache==1);
 assert(fat32_read_boot_sector(&d,b) && direct==9 && cached==6);
 fs.Device=0;assert(!fat32_read_sector(&fs,0,b) && !strcmp(tag,"FAT_SECTOR_RANGE_FAIL"));
}
'''
assert 'boot_filesystem' not in (root/'kernel/fs/nativefs/fat32.c').read_text()
with tempfile.TemporaryDirectory() as directory:
    path=Path(directory);(path/'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie',str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('FAT boot/cached reads: direct/cache selection, 3 retries, range/null failures and diagnostic fields passed')
