"""Exercise the actual pinned-source padding block, including invalid bounds."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'build/nvidia/vulkan/ffmpeg-7.1.1/libavcodec/vulkan_decode.c').read_text()
start = source.index('    sd_buf = (FFVkBuffer *)vp->slices_buf->data;')
end = source.index('    /* Flush if needed */', start)
block = source[start:end]
fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#define AVERROR(x) (-(x))
typedef struct { unsigned char *mapped_mem; size_t size; } FFVkBuffer;
typedef struct { unsigned char *data; } Ref;
typedef struct { Ref *slices_buf; size_t slices_size; } Picture;
static int pad(Picture *vp, size_t data_size) {
    FFVkBuffer *sd_buf;
BLOCK
    return 0;
}
int main(void) {
    unsigned char bytes[8200];
    FFVkBuffer buf={bytes,8192};Ref ref={(unsigned char*)&buf};Picture pic={&ref,0};
    const size_t alignments[]={1,16,256,4096};
    for(unsigned a=0;a<4;a++) for(size_t size=1;size<=8192;size+=31) {
        memset(bytes,0xa5,sizeof(bytes));pic.slices_size=size;
        size_t end=(size+alignments[a]-1)&~(alignments[a]-1);
        assert(pad(&pic,end)==0);
        for(size_t i=0;i<size;i++)assert(bytes[i]==0xa5);
        for(size_t i=size;i<end;i++)assert(bytes[i]==0);
        for(size_t i=end;i<sizeof(bytes);i++)assert(bytes[i]==0xa5);
    }
    memset(bytes,0xa5,sizeof(bytes));pic.slices_size=100;
    assert(pad(&pic,99)==-EINVAL);
    assert(pad(&pic,8193)==-EINVAL);
    for(size_t i=0;i<sizeof(bytes);i++)assert(bytes[i]==0xa5);
    buf.mapped_mem=0;assert(pad(&pic,256)==-EINVAL);
}
'''
with tempfile.TemporaryDirectory(prefix='video-padding-') as directory:
    base=Path(directory)
    for negative in (False, True):
        tested=block
        if negative:
            tested=tested.replace('memset(sd_buf->mapped_mem + vp->slices_size, 0,\n           data_size - vp->slices_size);', '(void)0;')
            assert tested!=block
        c=base/'test.c';binary=base/'test'
        c.write_text(fixture.replace('BLOCK',tested))
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(c),'-o',str(binary)],check=True)
        result=subprocess.run([str(binary)],capture_output=True,timeout=5)
        assert (result.returncode!=0)==negative
print('Video bitstream padding: payload/canaries, alignments, bounds and negative control passed')
