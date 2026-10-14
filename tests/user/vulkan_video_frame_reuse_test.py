"""Execute actual player download helper with allocation/lifetime accounting."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'tools/nvidia/vulkan_video_player.c').read_text()
helper = source[source.index('static int download_frame('):source.index('static int handle_window_event(')]
assert 'av_frame_unref(cpu)' not in source
assert 'av_frame_free(&cpu)' in source
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#define AVERROR(e) (-(e))
typedef struct {void *buf[1];int width,height;unsigned serial;} AVFrame;
static unsigned allocations,calls;
static int fail;
static int av_hwframe_transfer_data(AVFrame *dst,const AVFrame *src,int flags) {
    assert(!flags);++calls;
    if(fail)return -EIO;
    if(!dst->buf[0]){dst->buf[0]=malloc(16);assert(dst->buf[0]);++allocations;}
    dst->width=src->width;dst->height=src->height;dst->serial=src->serial;
    *(unsigned*)dst->buf[0]=src->serial;return 0;
}
''' + helper + r'''
int main(void) {
    AVFrame cpu={0},frame={.width=320,.height=180};
    for(unsigned i=0;i<10000;++i) {
        frame.serial=i;assert(!download_frame(&cpu,&frame));
        assert(cpu.serial==i&&*(unsigned*)cpu.buf[0]==i);
    }
    assert(allocations==1&&calls==10000);
    void *buffer=cpu.buf[0];fail=1;
    assert(download_frame(&cpu,&frame)==-EIO&&cpu.buf[0]==buffer);
    fail=0;frame.width=640;
    unsigned before=calls;
    assert(download_frame(&cpu,&frame)==-EINVAL&&calls==before&&cpu.buf[0]==buffer);
    frame.width=320;frame.height=360;
    assert(download_frame(&cpu,&frame)==-EINVAL&&calls==before);
    free(cpu.buf[0]);
}
'''
with tempfile.TemporaryDirectory(prefix='video-frame-reuse-') as directory:
    base = Path(directory)
    (base / 'test.c').write_text(code)
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(base / 'test.c'), '-o', str(base / 'test')], check=True)
    subprocess.run([str(base / 'test')], check=True, timeout=15)
print('Video download reuse: 10000 updated frames, one CPU allocation, transfer failure and resolution mismatch passed')
