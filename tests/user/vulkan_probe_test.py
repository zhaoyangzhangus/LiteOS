"""Compile the actual probe and test failures without accessing a host GPU."""
from pathlib import Path
import subprocess
import tempfile
import re

ROOT = Path(__file__).resolve().parents[2]
fixture = r'''
#include <vulkan/vulkan_core.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
int __real_fsync(int);
int __wrap_fsync(int fd) {
    if (getenv("PROBE_TEST_SYNC_FAILURE")) { errno=EIO; return -1; }
    return __real_fsync(fd);
}
static int mode, destroyed, live, drained, is_mapped;
VkResult __wrap_vkEnumerateDeviceExtensionProperties(VkPhysicalDevice p,const char *l,uint32_t *n,VkExtensionProperties *e) {
    assert(mode>=9 && !l && p==(VkPhysicalDevice)(uintptr_t)2);
    if(mode==12) return VK_ERROR_DEVICE_LOST;
    *n=1;
    if(e) {memset(e,0,sizeof(*e));strcpy(e->extensionName,mode==10 ? "VK_KHR_swapchain" : VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME);}
    return VK_SUCCESS;
}
static void video_queues(VkPhysicalDevice p,uint32_t *n,VkQueueFamilyProperties2 *f) {
    assert(p==(VkPhysicalDevice)(uintptr_t)2);*n=1;
    if(f) {
        f->queueFamilyProperties.queueCount=mode==11 ? 0 : 1;
        f->queueFamilyProperties.queueFlags=VK_QUEUE_VIDEO_DECODE_BIT_KHR;
        if(f->pNext) ((VkQueueFamilyVideoPropertiesKHR*)f->pNext)->videoCodecOperations=VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
    }
}
PFN_vkVoidFunction __wrap_vkGetInstanceProcAddr(VkInstance i,const char *n) {
    assert(mode>=9 && i==(VkInstance)(uintptr_t)1 && !strcmp(n,"vkGetPhysicalDeviceQueueFamilyProperties2"));
    return mode==13 ? NULL : (PFN_vkVoidFunction)video_queues;
}
int vulkan_shared_lifetime_probe(VkPhysicalDevice p,VkDevice d,VkQueue q,uint32_t f) {
    (void)p;(void)d;(void)q;(void)f;abort();
}
int vulkan_shared_support(VkPhysicalDevice p) {(void)p;abort();}
int vulkan_shared_probe(VkPhysicalDevice p,VkDevice d,VkQueue q,uint32_t f,int h) {
    (void)p;(void)d;(void)q;(void)f;(void)h;abort();
}
int vulkan_shared_render_probe(VkPhysicalDevice p,VkDevice d,VkQueue q,uint32_t f,int h) {
    (void)p;(void)d;(void)q;(void)f;(void)h;abort();
}
int vulkan_render_probe(VkPhysicalDevice p,VkDevice d,VkQueue q,uint32_t f,const char *path) {
    (void)p;(void)d;(void)q;(void)f;(void)path;
    assert(0 && "transfer regression must not invoke graphics");return 1;
}
static uint32_t data[1024], fill;
static void check_cleanup(void) { assert(destroyed == (mode != 1) && !live && !is_mapped); }
VkResult __wrap_vkCreateInstance(const VkInstanceCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkInstance *instance) {
    mode = atoi(getenv("PROBE_TEST_MODE"));
    assert(info->pApplicationInfo->apiVersion == (mode>=9 ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0) && !allocator);
    atexit(check_cleanup);
    if (mode == 1) return VK_ERROR_INCOMPATIBLE_DRIVER;
    *instance = (VkInstance)(uintptr_t)1;
    return VK_SUCCESS;
}
VkResult __wrap_vkEnumeratePhysicalDevices(VkInstance instance, uint32_t *count,
    VkPhysicalDevice *devices) {
    assert(instance == (VkInstance)(uintptr_t)1);
    *count = mode == 2 ? 0 : 1;
    if (devices) devices[0] = (VkPhysicalDevice)(uintptr_t)2;
    return VK_SUCCESS;
}
void __wrap_vkGetPhysicalDeviceProperties(VkPhysicalDevice device,
    VkPhysicalDeviceProperties *properties) {
    assert(device == (VkPhysicalDevice)(uintptr_t)2);
    memset(properties, 0, sizeof(*properties));
    properties->vendorID = mode >= 4 ? 0x10de : 0x10005;
    properties->deviceType = mode >= 4 ? VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU : VK_PHYSICAL_DEVICE_TYPE_CPU;
}
void __wrap_vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *allocator) {
    assert(instance == (VkInstance)(uintptr_t)1 && !allocator);
    ++destroyed;
}
void __wrap_vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice d, uint32_t *n, VkQueueFamilyProperties *p) {
    *n=1; if(p) { memset(p,0,sizeof(*p)); p->queueCount=1; p->queueFlags=VK_QUEUE_TRANSFER_BIT; }
}
VkResult __wrap_vkCreateDevice(VkPhysicalDevice p,const VkDeviceCreateInfo *i,const VkAllocationCallbacks *a,VkDevice *d) {
    assert(i->queueCreateInfoCount==1); *d=(VkDevice)(uintptr_t)3; live|=1; return VK_SUCCESS;
}
void __wrap_vkGetDeviceQueue(VkDevice d,uint32_t f,uint32_t i,VkQueue *q) { *q=(VkQueue)(uintptr_t)4; }
#define CREATE(name, Info, Handle, bit) \
VkResult __wrap_##name(VkDevice d,const Info *i,const VkAllocationCallbacks *a,Handle *h) { \
    *h=(Handle)(uintptr_t)(bit); live|=(bit); return VK_SUCCESS; }
CREATE(vkCreateBuffer,VkBufferCreateInfo,VkBuffer,2)
CREATE(vkAllocateMemory,VkMemoryAllocateInfo,VkDeviceMemory,4)
CREATE(vkCreateCommandPool,VkCommandPoolCreateInfo,VkCommandPool,8)
CREATE(vkCreateFence,VkFenceCreateInfo,VkFence,16)
void __wrap_vkGetBufferMemoryRequirements(VkDevice d,VkBuffer b,VkMemoryRequirements *r) {
    memset(r,0,sizeof(*r)); r->size=sizeof(data); r->memoryTypeBits=1;
}
void __wrap_vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice d,VkPhysicalDeviceMemoryProperties *p) {
    memset(p,0,sizeof(*p)); p->memoryTypeCount=1;
    p->memoryTypes[0].propertyFlags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
}
VkResult __wrap_vkBindBufferMemory(VkDevice d,VkBuffer b,VkDeviceMemory m,VkDeviceSize o) { assert(!o); return VK_SUCCESS; }
VkResult __wrap_vkAllocateCommandBuffers(VkDevice d,const VkCommandBufferAllocateInfo *i,VkCommandBuffer *c) {
    assert(i->commandBufferCount==1); *c=(VkCommandBuffer)(uintptr_t)5; return VK_SUCCESS;
}
VkResult __wrap_vkBeginCommandBuffer(VkCommandBuffer c,const VkCommandBufferBeginInfo *i) { return VK_SUCCESS; }
void __wrap_vkCmdFillBuffer(VkCommandBuffer c,VkBuffer b,VkDeviceSize o,VkDeviceSize size,uint32_t value) {
    assert(!o && size==sizeof(data)); fill=value;
}
void __wrap_vkCmdPipelineBarrier(VkCommandBuffer c,VkPipelineStageFlags src,VkPipelineStageFlags dst,
    VkDependencyFlags flags,uint32_t n,const VkMemoryBarrier *m,uint32_t bn,const VkBufferMemoryBarrier *b,
    uint32_t in,const VkImageMemoryBarrier *i) {
    assert(src==VK_PIPELINE_STAGE_TRANSFER_BIT && dst==VK_PIPELINE_STAGE_HOST_BIT && n==1 && !bn && !in);
    assert(m->srcAccessMask==VK_ACCESS_TRANSFER_WRITE_BIT && m->dstAccessMask==VK_ACCESS_HOST_READ_BIT);
}
VkResult __wrap_vkEndCommandBuffer(VkCommandBuffer c) { return VK_SUCCESS; }
VkResult __wrap_vkQueueSubmit(VkQueue q,uint32_t n,const VkSubmitInfo *s,VkFence f) {
    assert(n==1 && s->commandBufferCount==1 && fill==0x5070cafe);
    if(mode==6) return VK_ERROR_DEVICE_LOST;
    for(unsigned j=0;j<1024;j++) data[j]=fill;
    if(mode==5) data[1023]^=1;
    return VK_SUCCESS;
}
VkResult __wrap_vkWaitForFences(VkDevice d,uint32_t n,const VkFence *f,VkBool32 all,uint64_t timeout) {
    assert(n==1 && all && timeout==UINT64_C(5000000000));
    return mode==7 ? VK_TIMEOUT : VK_SUCCESS;
}
VkResult __wrap_vkMapMemory(VkDevice d,VkDeviceMemory m,VkDeviceSize o,VkDeviceSize size,VkMemoryMapFlags f,void **p) {
    assert(!o && size==sizeof(data)); if(mode==8) return VK_ERROR_MEMORY_MAP_FAILED;
    is_mapped=1; *p=data; return VK_SUCCESS;
}
VkResult __wrap_vkDeviceWaitIdle(VkDevice d) { drained=1; return VK_SUCCESS; }
void __wrap_vkUnmapMemory(VkDevice d,VkDeviceMemory m) { assert(is_mapped && drained); is_mapped=0; }
#define DESTROY(name, Handle, bit) \
void __wrap_##name(VkDevice d,Handle h,const VkAllocationCallbacks *a) { \
    assert(drained && (live & (bit))); live &= ~(bit); }
DESTROY(vkDestroyFence,VkFence,16)
DESTROY(vkDestroyCommandPool,VkCommandPool,8)
DESTROY(vkDestroyBuffer,VkBuffer,2)
DESTROY(vkFreeMemory,VkDeviceMemory,4)
void __wrap_vkDestroyDevice(VkDevice d,const VkAllocationCallbacks *a) { assert(live==1 && drained); live=0; }
'''
with tempfile.TemporaryDirectory(prefix='vulkan-probe-test-') as directory:
    base = Path(directory)
    (base/'fixture.c').write_text(fixture)
    binary = base/'probe'
    names = sorted(set(re.findall(r'\b(vk[A-Z]\w*)\(', (ROOT/'tools/nvidia/vulkan_probe.c').read_text())))
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', str(ROOT/'tools/nvidia/vulkan_probe.c'), str(base/'fixture.c'),
                    *['-Wl,--wrap='+name for name in names], '-lvulkan',
                    '-Wl,--wrap=fsync',
                    '-o', str(binary)], check=True)
    import os
    for mode, error in ((1, 'vkCreateInstance'), (2, 'no physical devices'),
                        (3, 'no NVIDIA discrete GPU'), (5, 'readback word=1023'),
                        (6, 'vkQueueSubmit'), (7, 'vkWaitForFences'), (8, 'vkMapMemory')):
        result = subprocess.run([str(binary)], env=dict(os.environ, PROBE_TEST_MODE=str(mode)),
                                text=True, capture_output=True, timeout=5)
        assert result.returncode == 1, result
        assert error in result.stderr, result.stderr
        assert 'VULKAN_TRANSFER_PASS' not in result.stdout
    result = subprocess.run([str(binary)], env=dict(os.environ, PROBE_TEST_MODE='4'),
                            text=True, capture_output=True, timeout=5)
    assert result.returncode == 0 and 'VULKAN_TRANSFER_PASS' in result.stdout, result
    logfile = base/'gpu.log'
    result = subprocess.run([str(binary), '--log', str(logfile)],
                            env=dict(os.environ, PROBE_TEST_MODE='4'),
                            text=True, capture_output=True, timeout=5)
    assert result.returncode == 0 and not result.stdout and not result.stderr, result
    assert 'VULKAN_START' in logfile.read_text()
    assert 'VULKAN_TRANSFER_PASS' in logfile.read_text()
    result = subprocess.run([str(binary), '--log', str(logfile)],
                            env=dict(os.environ, PROBE_TEST_MODE='4', PROBE_TEST_SYNC_FAILURE='1'),
                            text=True, capture_output=True, timeout=5)
    assert result.returncode == 1 and 'VULKAN_FAIL log sync' in logfile.read_text()
    result = subprocess.run([str(binary), '--log', str(base/'missing/gpu.log')],
                            text=True, capture_output=True, timeout=5)
    assert result.returncode == 2
    for mode in range(9,14):
        result = subprocess.run([str(binary), '--video-caps'],
                                env=dict(os.environ, PROBE_TEST_MODE=str(mode)),
                                text=True, capture_output=True, timeout=5)
        assert result.returncode == (0 if mode==9 else 1), result
        assert 'VULKAN_TRANSFER_PASS' not in result.stdout
        if mode==9: assert 'VULKAN_VIDEO_CAPS decode_queue=1' in result.stdout
    for arguments in (['--video-caps','--render'], ['--video-caps','--shared-window'], ['--video-caps','--video-caps']):
        result=subprocess.run([str(binary),*arguments],capture_output=True,timeout=5)
        assert result.returncode==2, result
print('Vulkan probe simulated API regression: success, corrupt readback, submit/map/fence '
      'failures, missing driver/device, software rejection and cleanup passed; no host GPU accessed')
