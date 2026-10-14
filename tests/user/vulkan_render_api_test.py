"""Actual graphics probe with all Vulkan APIs mocked: no host GPU is reachable."""
from pathlib import Path
import importlib.util
import os
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('vulkan_stage_api',root/'tools/nvidia/stage-vulkan.py')
stage=importlib.util.module_from_spec(spec);spec.loader.exec_module(stage)
fixture=r'''
#include <vulkan/vulkan_core.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
static uint64_t live;
static unsigned step,fail_at,allocations,shaders;
static int drained,mapped,drawn,submitted,barrier;
static uint8_t pixels[256*256*4];
static unsigned side=64;
static int target;
static VkResult result(void) {return ++step==fail_at?VK_ERROR_OUT_OF_HOST_MEMORY:VK_SUCCESS;}
#define CREATE(name,Info,Handle,bit) \
VkResult name(VkDevice d,const Info *i,const VkAllocationCallbacks *a,Handle *h) { \
 assert(d && i && !a); VkResult r=result();if(r)return r;live|=(bit);*h=(Handle)(uintptr_t)(bit);return VK_SUCCESS;}
CREATE(vkCreateImage,VkImageCreateInfo,VkImage,1)
CREATE(vkCreateImageView,VkImageViewCreateInfo,VkImageView,4)
CREATE(vkCreateRenderPass,VkRenderPassCreateInfo,VkRenderPass,8)
CREATE(vkCreateFramebuffer,VkFramebufferCreateInfo,VkFramebuffer,16)
CREATE(vkCreatePipelineLayout,VkPipelineLayoutCreateInfo,VkPipelineLayout,128)
CREATE(vkCreateBuffer,VkBufferCreateInfo,VkBuffer,512)
CREATE(vkCreateCommandPool,VkCommandPoolCreateInfo,VkCommandPool,2048)
CREATE(vkCreateFence,VkFenceCreateInfo,VkFence,4096)
VkResult vkCreateShaderModule(VkDevice d,const VkShaderModuleCreateInfo *i,const VkAllocationCallbacks *a,VkShaderModule *h) {
 assert(i->pCode[0]==0x07230203 && i->codeSize>20 && !a && d);VkResult r=result();if(r)return r;
 unsigned bit=shaders++?64:32;live|=bit;*h=(VkShaderModule)(uintptr_t)bit;return VK_SUCCESS;
}
VkResult vkCreateGraphicsPipelines(VkDevice d,VkPipelineCache c,uint32_t n,const VkGraphicsPipelineCreateInfo *i,
 const VkAllocationCallbacks *a,VkPipeline *h) {
 assert(d && !c && !a && n==1 && i->stageCount==2 && i->renderPass && i->layout);
 assert(i->pInputAssemblyState->topology==VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
 assert(i->pViewportState->pViewports[0].width==side && i->pViewportState->pViewports[0].height==side);
 assert(i->pRasterizationState->cullMode==VK_CULL_MODE_NONE);
 VkResult r=result();if(r)return r;live|=256;*h=(VkPipeline)(uintptr_t)256;return VK_SUCCESS;
}
void vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice p,VkFormat f,VkFormatProperties *v) {
 assert(p && f==(target?VK_FORMAT_B8G8R8A8_UNORM:VK_FORMAT_R8G8B8A8_UNORM));memset(v,0,sizeof(*v));
 v->optimalTilingFeatures=getenv("NO_FORMAT")?0:VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
}
void vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice p,VkPhysicalDeviceMemoryProperties *v) {
 assert(p);memset(v,0,sizeof(*v));v->memoryTypeCount=2;
 if(!getenv("NO_MEMORY")) {
 v->memoryTypes[0].propertyFlags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
 v->memoryTypes[1].propertyFlags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;}
}
void vkGetImageMemoryRequirements(VkDevice d,VkImage i,VkMemoryRequirements *r) {assert(d && i);*r=(VkMemoryRequirements){side*side*4U,4096,3};}
void vkGetBufferMemoryRequirements(VkDevice d,VkBuffer b,VkMemoryRequirements *r) {assert(!target && d && b);*r=(VkMemoryRequirements){side*side*4U,4096,3};}
VkResult vkAllocateMemory(VkDevice d,const VkMemoryAllocateInfo *i,const VkAllocationCallbacks *a,VkDeviceMemory *m) {
 assert(d && !a && i->allocationSize==side*side*4U && i->memoryTypeIndex==allocations);
 VkResult r=result();if(r)return r;unsigned bit=allocations++?1024:2;live|=bit;*m=(VkDeviceMemory)(uintptr_t)bit;return VK_SUCCESS;
}
VkResult vkBindImageMemory(VkDevice d,VkImage i,VkDeviceMemory m,VkDeviceSize o) {assert(d && i && m && !o);return result();}
VkResult vkBindBufferMemory(VkDevice d,VkBuffer b,VkDeviceMemory m,VkDeviceSize o) {assert(d && b && m && !o);return result();}
VkResult vkAllocateCommandBuffers(VkDevice d,const VkCommandBufferAllocateInfo *i,VkCommandBuffer *c) {
 assert(d && i->commandPool && i->commandBufferCount==1);VkResult r=result();if(r)return r;*c=(VkCommandBuffer)(uintptr_t)1;return VK_SUCCESS;
}
VkResult vkBeginCommandBuffer(VkCommandBuffer c,const VkCommandBufferBeginInfo *i) {assert(c && i);return result();}
void vkCmdBeginRenderPass(VkCommandBuffer c,const VkRenderPassBeginInfo *i,VkSubpassContents s) {
 assert(c && s==VK_SUBPASS_CONTENTS_INLINE && i->renderPass && i->framebuffer && i->clearValueCount==1);
 assert(i->pClearValues[0].color.float32[0]==0 && i->pClearValues[0].color.float32[3]==1);
}
void vkCmdBindPipeline(VkCommandBuffer c,VkPipelineBindPoint b,VkPipeline p) {assert(c && b==VK_PIPELINE_BIND_POINT_GRAPHICS && p);}
void vkCmdDraw(VkCommandBuffer c,uint32_t n,uint32_t instances,uint32_t first,uint32_t base) {
 assert(c && n==3 && instances==1 && !first && !base);drawn=1;
 for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x) {
  double X=x+.5,Y=y+.5;
  double low=side/8.,high=7*side/8.,mid=side/2.,height=high-low;
  int inside=height*(Y-low)>0 && -height*(Y-low)-2*height*(X-high)>0 &&
             -height*(Y-high)+2*height*(X-mid)>0;
  unsigned off=(y*side+x)*4;
  pixels[off+(target?2:0)]=inside?255:0;pixels[off+1]=inside?64:0;
  pixels[off+(target?0:2)]=0;pixels[off+3]=255;
 }
}
void vkCmdEndRenderPass(VkCommandBuffer c) {assert(c && drawn);}
void vkCmdCopyImageToBuffer(VkCommandBuffer c,VkImage i,VkImageLayout l,VkBuffer b,uint32_t n,const VkBufferImageCopy *v) {
 assert(c && i && b && drawn && l==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && n==1);
 assert(v->imageExtent.width==side && v->imageExtent.height==side && !v->bufferRowLength);
 if(target)assert(b==(VkBuffer)(uintptr_t)0x8000);
}
void vkCmdPipelineBarrier(VkCommandBuffer c,VkPipelineStageFlags s,VkPipelineStageFlags d,VkDependencyFlags f,
 uint32_t n,const VkMemoryBarrier *m,uint32_t bn,const VkBufferMemoryBarrier *b,uint32_t in,const VkImageMemoryBarrier *i) {
 assert(c && s==VK_PIPELINE_STAGE_TRANSFER_BIT && d==VK_PIPELINE_STAGE_HOST_BIT && !f && n==1);
 assert(m->srcAccessMask==VK_ACCESS_TRANSFER_WRITE_BIT && m->dstAccessMask==VK_ACCESS_HOST_READ_BIT && !bn && !in && !b && !i);barrier=1;
}
VkResult vkEndCommandBuffer(VkCommandBuffer c) {assert(c && drawn && barrier);return result();}
VkResult vkQueueSubmit(VkQueue q,uint32_t n,const VkSubmitInfo *i,VkFence f) {
 assert(q && n==1 && f && i->commandBufferCount==1 && drawn && barrier);VkResult r=result();if(!r)submitted=1;return r;
}
VkResult vkWaitForFences(VkDevice d,uint32_t n,const VkFence *f,VkBool32 all,uint64_t t) {
 assert(d && n==1 && f && all && submitted && t==5000000000ULL);
 if(target && getenv("CORRUPT"))pixels[side*side*4U-1]=0;
 return result();
}
VkResult vkMapMemory(VkDevice d,VkDeviceMemory m,VkDeviceSize o,VkDeviceSize n,VkMemoryMapFlags f,void **p) {
 assert(!target && d && m && !o && n==side*side*4U && !f && submitted);VkResult r=result();if(r)return r;
 mapped=1;*p=pixels;if(getenv("CORRUPT"))pixels[side*side*4U-1]=0;return VK_SUCCESS;
}
VkResult vkDeviceWaitIdle(VkDevice d) {assert(d);drained=1;return VK_SUCCESS;}
void vkUnmapMemory(VkDevice d,VkDeviceMemory m) {assert(d && m && mapped && drained);mapped=0;}
#define DESTROY(name,Handle) \
void name(VkDevice d,Handle h,const VkAllocationCallbacks *a) { \
 assert(d && !a && drained);uint64_t bit=(uintptr_t)h;assert(live & bit);live &= ~bit;}
DESTROY(vkDestroyImage,VkImage)
DESTROY(vkDestroyImageView,VkImageView)
DESTROY(vkDestroyRenderPass,VkRenderPass)
DESTROY(vkDestroyFramebuffer,VkFramebuffer)
DESTROY(vkDestroyShaderModule,VkShaderModule)
DESTROY(vkDestroyPipelineLayout,VkPipelineLayout)
DESTROY(vkDestroyPipeline,VkPipeline)
DESTROY(vkDestroyBuffer,VkBuffer)
DESTROY(vkDestroyCommandPool,VkCommandPool)
DESTROY(vkDestroyFence,VkFence)
DESTROY(vkFreeMemory,VkDeviceMemory)
int vulkan_render_probe(VkPhysicalDevice,VkDevice,VkQueue,uint32_t,const char*);
int vulkan_render_surface(VkPhysicalDevice,VkDevice,VkQueue,uint32_t,VkBuffer,const volatile uint8_t*,uint32_t);
int main(void) {
 const char *value=getenv("FAIL_AT");fail_at=value?(unsigned)atoi(value):0;
 target=getenv("TARGET")!=NULL;side=target?256:64;
 int r=target ? vulkan_render_surface((VkPhysicalDevice)(uintptr_t)1,(VkDevice)(uintptr_t)2,
   (VkQueue)(uintptr_t)3,0,(VkBuffer)(uintptr_t)0x8000,pixels,side) :
   vulkan_render_probe((VkPhysicalDevice)(uintptr_t)1,(VkDevice)(uintptr_t)2,(VkQueue)(uintptr_t)3,0,NULL);
 assert(!live && !mapped && drained);
 if(fail_at || getenv("CORRUPT") || getenv("NO_FORMAT") || getenv("NO_MEMORY")) assert(r==1);
 else assert(!r && step==(target?17U:21U));
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='vulkan-render-api-') as directory:
    base=Path(directory);stage.compile_shaders(base);(base/'mock.c').write_text(fixture)
    binary=base/'probe'
    subprocess.run(['cc','-std=c11','-D_POSIX_C_SOURCE=200809L','-O2','-Wall','-Wextra','-Werror',
                    '-I'+str(base),str(root/'tools/nvidia/vulkan_render_probe.c'),str(base/'mock.c'),
                    '-o',str(binary)],check=True)
    for overrides in [{}]+[{'FAIL_AT':str(i)} for i in range(1,22)]+[{'CORRUPT':'1'},{'NO_FORMAT':'1'},{'NO_MEMORY':'1'}]:
        result=subprocess.run([str(binary)],env=dict(os.environ,**overrides),capture_output=True,text=True,timeout=5)
        assert result.returncode==0,(overrides,result.stdout,result.stderr)
        assert ('VULKAN_RENDER_PASS' in result.stdout)==(not overrides)
    for overrides in [{}]+[{'FAIL_AT':str(i)} for i in range(1,18)]+[{'CORRUPT':'1'},{'NO_FORMAT':'1'},{'NO_MEMORY':'1'}]:
        result=subprocess.run([str(binary)],env=dict(os.environ,TARGET='1',**overrides),capture_output=True,text=True,timeout=5)
        assert result.returncode==0,(overrides,result.stdout,result.stderr)
        assert ('VULKAN_RENDER_PASS' in result.stdout)==(not overrides)
print('Vulkan graphics API: actual renderer, successful draw/readback, 21 API failure points, unsupported format/memory, corrupt pixels and complete cleanup passed; no host Vulkan library or GPU used')
print('Vulkan shared graphics: actual renderer, 65536 BGRA pixels, GPU copy to borrowed buffer, no mapping/ownership theft, 17 API failures and cleanup passed')
