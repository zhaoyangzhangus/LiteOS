"""Run the actual cross-library NV12 layout guard, including odd dimensions."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
code = r'''
#include <uapi/video_decoder.h>
#include <assert.h>
int main(void) {
 uint8_t pixel=0;
 os_video_frame_t f={.width=4,.height=2,.stride_y=4,.stride_uv=4,.y=&pixel,.uv=&pixel};
 assert(os_video_frame_layout_valid(&f));
 assert(!os_video_frame_layout_valid(0));
 f.width=3;f.stride_y=3;assert(os_video_frame_layout_valid(&f));
 f.stride_uv=3;assert(!os_video_frame_layout_valid(&f));f.stride_uv=4;
 f.y=0;assert(!os_video_frame_layout_valid(&f));f.y=&pixel;
 f.uv=0;assert(!os_video_frame_layout_valid(&f));f.uv=&pixel;
 f.stride_y=-1;assert(!os_video_frame_layout_valid(&f));f.stride_y=3;
 f.height=0;assert(!os_video_frame_layout_valid(&f));f.height=2;
 f.width=INT32_MAX;assert(!os_video_frame_layout_valid(&f));
 f.width=0;assert(!os_video_frame_layout_valid(&f));
 f.width=3;f.height=UINT32_MAX;assert(!os_video_frame_layout_valid(&f));
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'test.c').write_text(code)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-no-pie', '-I' + str(root / 'include'), str(path / 'test.c'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('NV12 shared ABI layout: odd-width chroma, null planes, negative strides and oversized/empty dimensions rejected')
