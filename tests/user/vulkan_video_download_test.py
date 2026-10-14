"""Exercise FFmpeg's actual patched copy routine without any GPU access."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'build/nvidia/vulkan/ffmpeg-7.1.1/libavutil/hwcontext_vulkan.c').read_text()
start=source.index('static int copy_buffer_data(')
end=source.index('\n}\n',start)+3
function=source[start:end]
fixture=r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <vulkan/vulkan_core.h>
#define AV_LOG_ERROR 1
#define AVERROR_EXTERNAL -999
typedef struct {uint8_t *mapped_mem;VkDeviceMemory mem;VkMemoryPropertyFlags flags;} FFVkBuffer;
typedef struct {uint8_t *data;} AVBufferRef;
typedef struct {uint8_t *data[4];int linesize[4];} AVFrame;
typedef struct {PFN_vkInvalidateMappedMemoryRanges InvalidateMappedMemoryRanges;PFN_vkFlushMappedMemoryRanges FlushMappedMemoryRanges;} FFVulkanFunctions;
typedef struct {VkDevice act_dev;} AVVulkanDeviceContext;
typedef struct {struct {FFVulkanFunctions vkfn;} vkctx;AVVulkanDeviceContext p;} VulkanDevicePriv;
typedef struct {struct {void *hwctx;} *device_ctx;} AVHWFramesContext;
static int invalidated,flushed,fail;
static VkResult invalidate(VkDevice d,uint32_t n,const VkMappedMemoryRange *m) {
    (void)d;assert(n==1&&m->size==VK_WHOLE_SIZE);++invalidated;return fail ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
}
static VkResult flush(VkDevice d,uint32_t n,const VkMappedMemoryRange *m) {
    (void)d;assert(n==1&&m->size==VK_WHOLE_SIZE);++flushed;return fail ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
}
static const char *ff_vk_ret2str(VkResult r){(void)r;return "error";}
static void av_log(void *p,int l,const char *f,const char *s){(void)p;(void)l;(void)f;(void)s;}
static void av_image_copy_plane(uint8_t *d,int ds,const uint8_t *s,int ss,int width,int height) {
    for(int y=0;y<height;++y)memcpy(d+y*ds,s+y*ss,width);
}
'''
test=r'''
int main(void) {
    VulkanDevicePriv priv={.vkctx.vkfn={invalidate,flush}};
    struct {void *hwctx;} dc={&priv};
    AVHWFramesContext ctx;ctx.device_ctx=(void*)&dc;
    uint8_t gpu[64],cpu0[8],cpu1[4];
    FFVkBuffer b={.mapped_mem=gpu};AVBufferRef ref={(uint8_t*)&b};
    AVFrame f={.data={cpu0,cpu1},.linesize={4,2}};
    VkBufferImageCopy regions[2]={
        {.bufferOffset=0,.bufferRowLength=8,.imageExtent.height=2},
        {.bufferOffset=32,.bufferRowLength=8,.imageExtent.height=2}};
    for(int coherent=0;coherent<2;++coherent) {
        b.flags=coherent ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : 0;
        memset(gpu,0x5a,sizeof(gpu));memset(cpu0,0,sizeof(cpu0));memset(cpu1,0,sizeof(cpu1));
        invalidated=flushed=0;
        assert(copy_buffer_data(&ctx,&ref,&f,regions,2,0)==0);
        for(unsigned i=0;i<sizeof(cpu0);++i)assert(cpu0[i]==0x5a);
        for(unsigned i=0;i<sizeof(cpu1);++i)assert(cpu1[i]==0x5a);
        for(unsigned i=0;i<sizeof(gpu);++i)assert(gpu[i]==0x5a);
        assert(invalidated==!coherent&&!flushed);
        memset(cpu0,0x33,sizeof(cpu0));memset(cpu1,0x66,sizeof(cpu1));
        assert(copy_buffer_data(&ctx,&ref,&f,regions,2,1)==0);
        assert(gpu[0]==0x33&&gpu[8]==0x33&&gpu[32]==0x66&&gpu[40]==0x66);
        assert(gpu[4]==0x5a&&gpu[34]==0x5a&&flushed==!coherent);
    }
    fail=1;b.flags=0;memset(cpu0,0,sizeof(cpu0));
    assert(copy_buffer_data(&ctx,&ref,&f,regions,2,0)==AVERROR_EXTERNAL&&cpu0[0]==0);
    assert(copy_buffer_data(&ctx,&ref,&f,regions,2,1)==AVERROR_EXTERNAL);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='vulkan-video-download-') as directory:
    base=Path(directory);c=base/'test.c';binary=base/'test'
    c.write_text(fixture+function+test)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(c),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=5)
print('Vulkan video download regression: upload/download direction, plane strides, coherence and errors passed')
