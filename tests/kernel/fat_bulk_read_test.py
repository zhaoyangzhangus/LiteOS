"""Production FAT reader and cache: fragmented extents and dirty coherence."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/nativefs/file_lifecycle.c').read_text()
begin = source.index('static BOOLEAN fat32_read(')
end = source.index('static BOOLEAN fat32_write_locked(', begin)
code = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <kernel/fat32.h>
static unsigned reads, max_count;
static BOOLEAN fail_read;
static BOOLEAN disk_read(VOID *ctx, UINT64 lba, UINT32 count, VOID *buffer) {
    (void)ctx; ++reads; if (count > max_count) max_count = count;
    if (fail_read) return 0;
    for (unsigned i=0;i<count;++i)
        memset((UINT8*)buffer+512*i,(int)((lba+i)&255),512);
    return 1;
}
static BOOLEAN disk_write(VOID *ctx, UINT64 lba, UINT32 count, const VOID *buffer) {
    (void)ctx;(void)lba;(void)count;(void)buffer;return 1;
}
static VOID fat32_diag_failure(const char *tag, UINT64 a, UINT64 b, UINT64 c) {
    (void)tag;(void)a;(void)b;(void)c;
}
static UINT64 fat32_cluster_lba(const OS_FAT32 *fs, UINT32 cluster) {
    return fs->DataStartLba+(UINT64)(cluster-2)*fs->SectorsPerCluster;
}
static BOOLEAN locate_file_cluster(OS_FAT32_FILE *file, UINT32 logical,
                                   UINT32 *physical) {
    file->CursorValid=1;file->CursorLogicalStart=(logical/4)*4;
    file->CursorPhysicalStart=2+(logical/4)*8;file->CursorLength=4;
    *physical=file->CursorPhysicalStart+logical%4;return 1;
}
static BOOLEAN fat32_read_sector(const OS_FAT32 *fs, UINT64 lba, UINT8 *buffer) {
    return block_cache_read((OS_BLOCK_CACHE*)&fs->Cache,lba,buffer);
}
''' + source[begin:end] + r'''
int main(void) {
    OS_BLOCK_DEVICE disk={.Registered=1,.BlockSize=512,.BlockCount=4096,
                             .Read=disk_read,.Write=disk_write};
    OS_FAT32 fs={.Device=&disk,.BytesPerSector=512,.SectorsPerCluster=32,
                    .DataStartLba=64};
    assert(block_cache_init(&fs.Cache,&disk));
    UINT8 *out=malloc(128*512+2);assert(out);
    UINT8 dirty[512];memset(dirty,0xa5,sizeof(dirty));
    assert(block_cache_write(&fs.Cache,100,dirty));
    reads=0;
    assert(block_cache_read_range(&fs.Cache,64,128,out));
    assert(reads==2 && out[(100-64)*512]==0xa5);
    for(unsigned i=0;i<128;++i)
        assert(out[i*512]==(i==36?0xa5:((64+i)&255)));
    assert(!block_cache_read_range(&fs.Cache,64,129,out));
    assert(!block_cache_read_range(&fs.Cache,4095,2,out));
    assert(!block_cache_read_range(&fs.Cache,UINT64_MAX,2,out));
    assert(!block_cache_read_range(&fs.Cache,64,0,out));
    fail_read=1;assert(!block_cache_read_range(&fs.Cache,200,2,out));
    fail_read=0;
    /* A failed backend read must release the lock and preserve dirty data. */
    assert(block_cache_read_range(&fs.Cache,100,1,out) && out[0]==0xa5);
    assert(block_cache_flush(&fs.Cache));
    assert(block_cache_destroy(&fs.Cache));
    assert(block_cache_init(&fs.Cache,&disk));
    OS_FAT32_FILE file={.FileSystem=&fs,.Used=1,.Size=19*16384+99};
    const unsigned capacities[]={1,511,512,513,4097,65536};
    for(unsigned offset=0;offset<1024;offset+=31)
        for(unsigned i=0;i<sizeof(capacities)/sizeof(capacities[0]);++i) {
            unsigned cap=capacities[i];UINT32 got=0;
            memset(out,0xcc,128*512+2);
            assert(fat32_read(&file,offset,out+1,cap,&got) && got==cap);
            assert(out[0]==0xcc && out[cap+1]==0xcc);
            for(unsigned j=0;j<got;++j) {
                UINT64 pos=offset+j;unsigned logical=pos/16384;
                UINT64 lba=64+(logical/4)*8*32+(logical%4)*32+(pos%16384)/512;
                assert(out[j+1]==(lba&255));
            }
        }
    UINT32 got;
    assert(fat32_read(&file,file.Size-31,out,512,&got) && got==31);
    assert(fat32_read(&file,file.Size,out,512,&got) && got==0);
    fail_read=1;
    assert(!fat32_read(&file,8*16384,out,65536,&got));
    fail_read=0;
    assert(fat32_read(&file,8*16384,out,65536,&got) && got==65536);
    assert(max_count==128);
    free(out);assert(block_cache_destroy(&fs.Cache));
    puts("FAT bulk reads: dirty coherence, fragmentation, unaligned tails, EOF, failure and 128-sector bound passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-fat-bulk-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root/'include'),'-x','c','-',
                    str(root/'kernel/block/bio.c'),
                    str(root/'kernel/fs/pagecache/cache.c'),'-o',binary],
                   input=code,text=True,check=True)
    subprocess.run([binary],check=True)
