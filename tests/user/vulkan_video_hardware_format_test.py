"""Execute the actual format-selection callback: never choose CPU fallback."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'tools/nvidia/vulkan_video_decoder.c').read_text()
callback = source[source.index('static enum AVPixelFormat select_hardware('):
                  source.index('uint32_t os_video_decoder_abi(')]
fixture = r'''
#include <assert.h>
typedef struct { int unused; } AVCodecContext;
enum AVPixelFormat { AV_PIX_FMT_NONE = -1, AV_PIX_FMT_NV12 = 0,
                     AV_PIX_FMT_YUV420P = 1, AV_PIX_FMT_VULKAN = 2 };
'''
checks = r'''
int main(void) {
    const enum AVPixelFormat software[] = {AV_PIX_FMT_NV12, AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE};
    const enum AVPixelFormat hardware[] = {AV_PIX_FMT_NV12, AV_PIX_FMT_VULKAN, AV_PIX_FMT_NONE};
    const enum AVPixelFormat empty[] = {AV_PIX_FMT_NONE};
    assert(select_hardware(0, software) == AV_PIX_FMT_NONE);
    assert(select_hardware(0, hardware) == AV_PIX_FMT_VULKAN);
    assert(select_hardware(0, empty) == AV_PIX_FMT_NONE);
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'test.c').write_text(fixture + callback + checks)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-no-pie', str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('Actual Vulkan format callback rejects software-only and empty format lists')
