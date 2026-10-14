"""Run actual lifetime probe with GPU/window APIs mocked, never host GPU access."""
from pathlib import Path
import os
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
fixture=r'''
#include <vulkan/vulkan_core.h>
#include <uapi/linux_abi.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>
static unsigned live,step,fail_at,unmapped,removed,closed,drained,mapped,submitted,source_barrier,host_barrier;
static uint32_t source[65536],pixels[65536];
static VkResult result(void){return ++step==fail_at?VK_ERROR_OUT_OF_DEVICE_MEMORY:VK_SUCCESS;}
int munmap(void *a,size_t n){assert(a==(void*)0x08000000 && n==262144 && !unmapped && live==15);
 if(getenv("NO_UNMAP"))return -1;
 unmapped=1;return 0;}
int ioctl(int fd,unsigned long request,...){assert(fd==7 && request==LINUX_WINDOW_IOCTL_COMMIT && unmapped && !removed);
 va_list ap;va_start(ap,request);linux_window_command_t *c=va_arg(ap,linux_window_command_t*);va_end(ap);
 assert(c->operation==LINUX_WINDOW_OPERATION_CLOSE && !c->address && !c->width);
 if(getenv("NO_REMOVE"))return -1;
 removed=1;return 0;}
int close(int fd){assert(fd==7 && unmapped && removed && !closed);closed=1;return getenv("NO_CLOSE")?-1:0;}
#define CREATE(name,Info,Handle,bit) \
VkResult name(VkDevice d,const Info *i,const VkAllocationCallbacks *a,Handle *h){ \
 assert(d && i && !a && !unmapped);VkResult r=result();if(r)return r;live|=bit;*h=(Handle)(uintptr_t)bit;return VK_SUCCESS;}
CREATE(vkCreateBuffer,VkBufferCreateInfo,VkBuffer,1)
CREATE(vkCreateCommandPool,VkCommandPoolCreateInfo,VkCommandPool,4)
CREATE(vkCreateFence,VkFenceCreateInfo,VkFence,8)
void vkGetBufferMemoryRequirements(VkDevice d,VkBuffer b,VkMemoryRequirements *r){assert(d && b);*r=(VkMemoryRequirements){262144,4096,1};}
void vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice p,VkPhysicalDeviceMemoryProperties *v){
 assert(p);memset(v,0,sizeof(*v));v->memoryTypeCount=1;
 v->memoryTypes[0].propertyFlags=getenv("NO_TYPE")?0:VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;}
VkResult vkAllocateMemory(VkDevice d,const VkMemoryAllocateInfo *i,const VkAllocationCallbacks *a,VkDeviceMemory *m){
 assert(d && !a && !i->pNext && !i->memoryTypeIndex && i->allocationSize==262144);
 VkResult r=result();if(r)return r;live|=2;*m=(VkDeviceMemory)(uintptr_t)2;return VK_SUCCESS;}
VkResult vkBindBufferMemory(VkDevice d,VkBuffer b,VkDeviceMemory m,VkDeviceSize o){assert(d && b && m && !o);return result();}
VkResult vkAllocateCommandBuffers(VkDevice d,const VkCommandBufferAllocateInfo *i,VkCommandBuffer *c){
 assert(d && i->commandPool && i->commandBufferCount==1);VkResult r=result();if(r)return r;*c=(VkCommandBuffer)(uintptr_t)1;return VK_SUCCESS;}
VkResult vkBeginCommandBuffer(VkCommandBuffer c,const VkCommandBufferBeginInfo *i){
 assert(c && i->flags==VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT && unmapped && removed && closed);return result();}
void vkCmdFillBuffer(VkCommandBuffer c,VkBuffer b,VkDeviceSize o,VkDeviceSize n,uint32_t value){
 assert(c && b==(VkBuffer)(uintptr_t)0x8000 && !o && n==262144 && value==0x5070ac1d && unmapped && removed && closed);
 for(unsigned j=0;j<65536;++j)source[j]=value;}
void vkCmdPipelineBarrier(VkCommandBuffer c,VkPipelineStageFlags s,VkPipelineStageFlags d,VkDependencyFlags flags,
 uint32_t n,const VkMemoryBarrier *m,uint32_t bn,const VkBufferMemoryBarrier *b,uint32_t in,const VkImageMemoryBarrier *i){
 assert(c && s==VK_PIPELINE_STAGE_TRANSFER_BIT && !flags && !in && !i);
 if(d==VK_PIPELINE_STAGE_TRANSFER_BIT){assert(!n && !m && bn==1 && b->buffer==(VkBuffer)(uintptr_t)0x8000);
 assert(b->srcAccessMask==VK_ACCESS_TRANSFER_WRITE_BIT && b->dstAccessMask==VK_ACCESS_TRANSFER_READ_BIT);
 assert(b->srcQueueFamilyIndex==VK_QUEUE_FAMILY_IGNORED && b->dstQueueFamilyIndex==VK_QUEUE_FAMILY_IGNORED);
 assert(!b->offset && b->size==262144);source_barrier=1;}
 else {assert(d==VK_PIPELINE_STAGE_HOST_BIT && n==1 && m && !bn && !b);
 assert(m->srcAccessMask==VK_ACCESS_TRANSFER_WRITE_BIT && m->dstAccessMask==VK_ACCESS_HOST_READ_BIT);host_barrier=1;}}
void vkCmdCopyBuffer(VkCommandBuffer c,VkBuffer src,VkBuffer dst,uint32_t n,const VkBufferCopy *copy){
 assert(c && src==(VkBuffer)(uintptr_t)0x8000 && dst==(VkBuffer)(uintptr_t)1 && n==1 && source_barrier);
 assert(copy->size==262144 && !copy->srcOffset && !copy->dstOffset);memcpy(pixels,source,sizeof(pixels));}
VkResult vkEndCommandBuffer(VkCommandBuffer c){assert(c && source_barrier && host_barrier);return result();}
VkResult vkQueueSubmit(VkQueue q,uint32_t n,const VkSubmitInfo *i,VkFence f){
 assert(q && n==1 && i->commandBufferCount==1 && f && unmapped && closed);VkResult r=result();if(!r)submitted=1;return r;}
VkResult vkWaitForFences(VkDevice d,uint32_t n,const VkFence *f,VkBool32 all,uint64_t timeout){
 assert(d && n==1 && f && all && timeout==5000000000ULL && submitted);return result();}
VkResult vkMapMemory(VkDevice d,VkDeviceMemory m,VkDeviceSize o,VkDeviceSize n,VkMemoryMapFlags f,void **p){
 assert(d && m==(VkDeviceMemory)(uintptr_t)2 && !o && n==262144 && !f && submitted);
 VkResult r=result();if(r)return r;mapped=1;*p=pixels;if(getenv("CORRUPT"))pixels[65535]^=1;return VK_SUCCESS;}
VkResult vkDeviceWaitIdle(VkDevice d){assert(d);drained=1;return VK_SUCCESS;}
void vkUnmapMemory(VkDevice d,VkDeviceMemory m){assert(d && m && mapped && drained);mapped=0;}
#define DESTROY(name,Handle,bit) \
void name(VkDevice d,Handle h,const VkAllocationCallbacks *a){ \
 assert(d && !a && drained && (uintptr_t)h==bit && (live&bit));live&=~bit;}
DESTROY(vkDestroyBuffer,VkBuffer,1U)
DESTROY(vkFreeMemory,VkDeviceMemory,2U)
DESTROY(vkDestroyCommandPool,VkCommandPool,4U)
DESTROY(vkDestroyFence,VkFence,8U)
int vulkan_import_lifetime(VkPhysicalDevice,VkDevice,VkQueue,uint32_t,VkBuffer,uint64_t,uint32_t,int*);
int main(void){const char *s=getenv("FAIL_AT");fail_at=s?(unsigned)atoi(s):0;int fd=7;
 int r=vulkan_import_lifetime((VkPhysicalDevice)(uintptr_t)1,(VkDevice)(uintptr_t)2,(VkQueue)(uintptr_t)3,
  0,(VkBuffer)(uintptr_t)0x8000,0x08000000,262144,&fd);
 assert(!live && !mapped && drained && fd==(closed?-1:7));
 if(getenv("EXPECT_FAIL"))assert(r==1);
 else assert(r==0 && step==11 && unmapped && removed && closed && submitted);
}
'''
with tempfile.TemporaryDirectory(prefix='vulkan-import-lifetime-') as directory:
    base=Path(directory);(base/'mock.c').write_text(fixture)
    binary=base/'probe'
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-I'+str(root/'include'),
                    str(root/'tools/nvidia/vulkan_import_lifetime_probe.c'),str(base/'mock.c'),'-o',str(binary)],check=True)
    modes=[{}]+[{'FAIL_AT':str(i),'EXPECT_FAIL':'1'} for i in range(1,12)]
    modes += [{name:'1','EXPECT_FAIL':'1'} for name in ('NO_UNMAP','NO_REMOVE','NO_CLOSE','NO_TYPE','CORRUPT')]
    for mode in modes:
        result=subprocess.run([str(binary)],env=dict(os.environ,**mode),capture_output=True,text=True,timeout=5)
        assert result.returncode==0,(mode,result.stdout,result.stderr)
        assert ('VULKAN_LIFETIME_PASS' in result.stdout)==(not mode)
print('Vulkan import lifetime: actual detach/fill/GPU-copy/barriers/fence/readback, 65536 words, 11 API failures, unmap/close/type/corruption failures and borrowed-owner cleanup passed; no host GPU used')
