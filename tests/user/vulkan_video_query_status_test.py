"""Verify the actual decoder log classifies signed Vulkan status by its sign."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'build/nvidia/vulkan/ffmpeg-7.1.1/libavcodec/vulkan_decode.c').read_text()
assert 'int32_t *result;' in source
end = source.index('\n    }\n', source.index('"Result of previous frame decoding:'))
start = source.rfind('        if (ret == VK_SUCCESS)', 0, end)
assert start > 0
snippet = source[start:end]
fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#define VK_SUCCESS 0
#define VK_NOT_READY 1
#define VK_QUERY_RESULT_STATUS_COMPLETE_KHR 1
#define AV_LOG_ERROR 16
#define AV_LOG_DEBUG 48
static int calls, level;
static int32_t logged;
static void av_log(void *context,int severity,const char *format,...) {
 (void)context;assert(strcmp(format,"Result of previous frame decoding: %d\n")==0 ||
                     strcmp(format,"Result of previous frame decoding: %u\n")==0);
 va_list args;va_start(args,format);logged=va_arg(args,int);va_end(args);
 level=severity;calls++;
}
static void report(int ret,int32_t *result) { void *avctx=0;
'''
test = r'''
}
int main(void) {
 int32_t statuses[]={1,1000331003,INT32_MAX,0,-1,-1000299000,INT32_MIN};
 for(unsigned i=0;i<sizeof(statuses)/sizeof(statuses[0]);i++) {
  calls=0;report(VK_SUCCESS,&statuses[i]);
  assert(calls==1&&logged==statuses[i]);
  assert(level==(statuses[i]>0 ? AV_LOG_DEBUG : AV_LOG_ERROR));
 }
 calls=0;report(VK_NOT_READY,0);report(-4,0);assert(calls==0);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='vulkan-status-test-') as directory:
    base = Path(directory)
    (base / 'test.c').write_text(fixture + snippet + test)
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=undefined,address', str(base / 'test.c'),
                    '-o', str(base / 'test')], check=True)
    subprocess.run([str(base / 'test')], check=True, timeout=5)
print('Actual Vulkan decoder status: all positive values succeed; zero/negative retain error diagnostics; unavailable results are not read')
