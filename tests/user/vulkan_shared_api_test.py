"""Real shared-surface probe, all GPU and guest-window operations mocked."""
from pathlib import Path
import os
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
fixture=r'''
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <vulkan/vulkan_core.h>
#include <uapi/linux_abi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>
static uint64_t live;
static unsigned step,fail_at,closed,presents,submits;
static int drained;
static uint32_t *surface;
int vulkan_import_lifetime(VkPhysicalDevice p,VkDevice d,VkQueue q,uint32_t f,
 VkBuffer b,uint64_t a,uint32_t n,int *fd) {
 assert(getenv("LIFETIME") && p && d && q && !f && b &&
        a==(uintptr_t)surface && n==262144 && fd && *fd==7 && live==3);
 if(getenv("NO_LIFETIME"))return 1;
 extern int close(int);
 *fd=-1;assert(!close(7));puts("VULKAN_LIFETIME_PASS mock");return 0;
}
int vulkan_render_surface(VkPhysicalDevice p,VkDevice d,VkQueue q,uint32_t f,
 VkBuffer b,const volatile uint8_t *pixels,uint32_t side) {
 assert(getenv("RENDER") && p && d && q && !f && b &&
        pixels==(const volatile uint8_t*)surface && side==256 && live==3);
 if(getenv("NO_DRAW"))return 1;
 ++submits;return 0; /* The renderer itself has a separate actual-API suite. */
}
static VkResult result(void) {return ++step==fail_at?VK_ERROR_OUT_OF_DEVICE_MEMORY:VK_SUCCESS;}
long syscall(long n,...) {
 assert(n==LINUX_SYS_WINDOW_CREATE && !surface);
 va_list a;va_start(a,n);linux_window_create_t *w=va_arg(a,linux_window_create_t*);va_end(a);
 assert(w->width==256 && w->height==256 && w->address==0x08000000);
 if(getenv("NO_WINDOW"))return -1;
 surface=aligned_alloc(65536,262144);assert(surface);memset(surface,0,262144);
 w->address=(uintptr_t)surface;w->buffer_size=262144;w->identifier=42;return 7;
}
int close(int fd) {
 assert(fd==7 && surface && !closed);
 if(getenv("LIFETIME") && live==3) {++closed;return 0;}
 assert(!live && drained);free(surface);surface=NULL;++closed;return 0;
}
int ioctl(int fd,unsigned long request,...) {
 assert(fd==7 && request==LINUX_WINDOW_IOCTL_COMMIT);
 va_list a;va_start(a,request);linux_window_command_t *c=va_arg(a,linux_window_command_t*);va_end(a);
 if(getenv("NO_PRESENT"))return -1;
 if(c->operation==LINUX_WINDOW_OPERATION_PRESENT) {
 assert(submits && c->width==256 && c->height==256 && c->stride==1024 && c->length==262144);
 assert(c->address==(uintptr_t)surface);++presents;
 } else assert(c->operation==LINUX_WINDOW_OPERATION_VISIBILITY && c->width==1);
 return 0;
}
VkResult vkEnumerateDeviceExtensionProperties(VkPhysicalDevice p,const char *layer,uint32_t *n,VkExtensionProperties *v) {
 assert(p && !layer);*n=1;
 if(getenv("NO_EXT"))return VK_ERROR_EXTENSION_NOT_PRESENT;
 if(v)strcpy(v->extensionName,VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
 return VK_SUCCESS;
}
void vkGetPhysicalDeviceProperties2(VkPhysicalDevice p,VkPhysicalDeviceProperties2 *v) {
 assert(p && v->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2);
 VkPhysicalDeviceExternalMemoryHostPropertiesEXT *h=v->pNext;
 assert(h->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT);
 h->minImportedHostPointerAlignment=getenv("BAD_ALIGN")?1048576:4096;
}
void vkGetPhysicalDeviceExternalBufferProperties(VkPhysicalDevice p,const VkPhysicalDeviceExternalBufferInfo *i,VkExternalBufferProperties *v) {
 assert(p && i->usage==(VK_BUFFER_USAGE_TRANSFER_DST_BIT | (getenv("LIFETIME")?VK_BUFFER_USAGE_TRANSFER_SRC_BIT:0)) && i->handleType==VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT);
 v->externalMemoryProperties.externalMemoryFeatures=getenv("NO_IMPORT")?0:VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
 v->externalMemoryProperties.compatibleHandleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
}
VkResult vkGetMemoryHostPointerPropertiesEXT(VkDevice d,VkExternalMemoryHandleTypeFlagBits h,const void *p,VkMemoryHostPointerPropertiesEXT *v) {
 assert(d && h==VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT && p==surface);
 VkResult r=result();if(r)return r;v->memoryTypeBits=1;return VK_SUCCESS;
}
PFN_vkVoidFunction vkGetDeviceProcAddr(VkDevice d,const char *name) {
 assert(d && !strcmp(name,"vkGetMemoryHostPointerPropertiesEXT"));
 return getenv("NO_PROC")?NULL:(PFN_vkVoidFunction)vkGetMemoryHostPointerPropertiesEXT;
}
VkResult vkCreateBuffer(VkDevice d,const VkBufferCreateInfo *i,const VkAllocationCallbacks *a,VkBuffer *b) {
 assert(d && !a && i->size==262144 && i->usage==(VK_BUFFER_USAGE_TRANSFER_DST_BIT | (getenv("LIFETIME")?VK_BUFFER_USAGE_TRANSFER_SRC_BIT:0)));
 const VkExternalMemoryBufferCreateInfo *e=i->pNext;
 assert(e->handleTypes==VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT);
 VkResult r=result();if(r)return r;live|=1;*b=(VkBuffer)(uintptr_t)1;return VK_SUCCESS;
}
void vkGetBufferMemoryRequirements(VkDevice d,VkBuffer b,VkMemoryRequirements *r) {
 assert(d && b);*r=(VkMemoryRequirements){getenv("BIG_REQ")?524288:262144,4096,1};
}
void vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice p,VkPhysicalDeviceMemoryProperties *v) {
 assert(p);memset(v,0,sizeof(*v));v->memoryTypeCount=1;
 v->memoryTypes[0].propertyFlags=getenv("NO_TYPE")?0:VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
}
VkResult vkAllocateMemory(VkDevice d,const VkMemoryAllocateInfo *i,const VkAllocationCallbacks *a,VkDeviceMemory *m) {
 assert(d && !a && i->allocationSize==262144 && i->memoryTypeIndex==0);
 const VkImportMemoryHostPointerInfoEXT *h=i->pNext;assert(h && h->pHostPointer==surface);
 assert(h->handleType==VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT);
 const VkMemoryDedicatedAllocateInfo *specific=h->pNext;assert(specific && specific->buffer==(VkBuffer)(uintptr_t)1 && !specific->image);
 VkResult r=result();if(r)return r;live|=2;*m=(VkDeviceMemory)(uintptr_t)2;return VK_SUCCESS;
}
VkResult vkBindBufferMemory(VkDevice d,VkBuffer b,VkDeviceMemory m,VkDeviceSize o) {assert(d && b && m && !o);return result();}
#define CREATE(name,Info,Handle,bit) \
VkResult name(VkDevice d,const Info *i,const VkAllocationCallbacks *a,Handle *h) { \
 assert(d && i && !a);VkResult r=result();if(r)return r;live|=bit;*h=(Handle)(uintptr_t)bit;return VK_SUCCESS;}
CREATE(vkCreateCommandPool,VkCommandPoolCreateInfo,VkCommandPool,4)
CREATE(vkCreateFence,VkFenceCreateInfo,VkFence,8)
VkResult vkAllocateCommandBuffers(VkDevice d,const VkCommandBufferAllocateInfo *i,VkCommandBuffer *c) {
 assert(d && i->commandPool && i->commandBufferCount==1);VkResult r=result();if(r)return r;*c=(VkCommandBuffer)(uintptr_t)1;return VK_SUCCESS;
}
VkResult vkBeginCommandBuffer(VkCommandBuffer c,const VkCommandBufferBeginInfo *i) {assert(c && i);return result();}
void vkCmdFillBuffer(VkCommandBuffer c,VkBuffer b,VkDeviceSize o,VkDeviceSize n,uint32_t value) {
 assert(c && b && !o && n==262144 && value==0xffff4000);
 for(unsigned i=0;i<65536;++i)surface[i]=value;
 if(getenv("CORRUPT"))surface[65535]^=1;
}
void vkCmdPipelineBarrier(VkCommandBuffer c,VkPipelineStageFlags s,VkPipelineStageFlags d,VkDependencyFlags f,
 uint32_t n,const VkMemoryBarrier *m,uint32_t bn,const VkBufferMemoryBarrier *b,uint32_t in,const VkImageMemoryBarrier *i) {
 assert(c && s==VK_PIPELINE_STAGE_TRANSFER_BIT && d==VK_PIPELINE_STAGE_HOST_BIT && !f && n==1);
 assert(m->srcAccessMask==VK_ACCESS_TRANSFER_WRITE_BIT && m->dstAccessMask==VK_ACCESS_HOST_READ_BIT && !bn && !in && !b && !i);
}
VkResult vkEndCommandBuffer(VkCommandBuffer c) {assert(c);return result();}
VkResult vkQueueSubmit(VkQueue q,uint32_t n,const VkSubmitInfo *i,VkFence f) {
 assert(q && n==1 && f && i->commandBufferCount==1);VkResult r=result();if(!r)++submits;return r;
}
VkResult vkWaitForFences(VkDevice d,uint32_t n,const VkFence *f,VkBool32 all,uint64_t t) {
 assert(d && n==1 && f && all && submits && t==5000000000ULL);return result();
}
VkResult vkDeviceWaitIdle(VkDevice d) {assert(d);drained=1;return VK_SUCCESS;}
void vkDestroyFence(VkDevice d,VkFence h,const VkAllocationCallbacks *a) {assert(d && !a && drained && (live&8) && surface && h);live&=~8ULL;}
void vkDestroyCommandPool(VkDevice d,VkCommandPool h,const VkAllocationCallbacks *a) {assert(d && !a && drained && (live&4) && surface && h);live&=~4ULL;}
void vkDestroyBuffer(VkDevice d,VkBuffer h,const VkAllocationCallbacks *a) {assert(d && !a && drained && (live&1) && surface && h);live&=~1ULL;}
void vkFreeMemory(VkDevice d,VkDeviceMemory h,const VkAllocationCallbacks *a) {
 assert(d && !a && drained && (live&2) && surface && h);live&=~2ULL;
 if(getenv("LIFETIME") && closed) {assert(!live);free(surface);surface=NULL;}
}
int vulkan_shared_support(VkPhysicalDevice);
int vulkan_shared_probe(VkPhysicalDevice,VkDevice,VkQueue,uint32_t,int);
int vulkan_shared_render_probe(VkPhysicalDevice,VkDevice,VkQueue,uint32_t,int);
int vulkan_shared_lifetime_probe(VkPhysicalDevice,VkDevice,VkQueue,uint32_t);
int main(void) {
 const char *value=getenv("FAIL_AT");fail_at=value?(unsigned)atoi(value):0;
 int supported=vulkan_shared_support((VkPhysicalDevice)(uintptr_t)1);
 if(getenv("NO_EXT")){assert(!supported);return 0;}assert(supported);
 int r=getenv("LIFETIME") ? vulkan_shared_lifetime_probe((VkPhysicalDevice)(uintptr_t)1,(VkDevice)(uintptr_t)2,(VkQueue)(uintptr_t)3,0) :
  getenv("RENDER") ? vulkan_shared_render_probe((VkPhysicalDevice)(uintptr_t)1,(VkDevice)(uintptr_t)2,(VkQueue)(uintptr_t)3,0,0) :
  vulkan_shared_probe((VkPhysicalDevice)(uintptr_t)1,(VkDevice)(uintptr_t)2,(VkQueue)(uintptr_t)3,0,0);
 assert(!live && !surface && drained);
 if(getenv("EXPECT_FAIL"))assert(r==1 && !presents);
 else if(getenv("LIFETIME"))assert(r==0 && !presents && !submits && closed==1 && step==4);
 else assert(r==0 && presents==1 && submits==1 && closed==1 && step==(getenv("RENDER")?4U:11U));
}
'''
with tempfile.TemporaryDirectory(prefix='vulkan-shared-api-') as directory:
    base=Path(directory);(base/'mock.c').write_text(fixture)
    binary=base/'probe'
    subprocess.run(['cc','-std=c11','-D_GNU_SOURCE','-O2','-Wall','-Wextra','-Werror',
                    '-I'+str(root/'include'),str(root/'tools/nvidia/vulkan_shared_probe.c'),str(base/'mock.c'),
                    '-o',str(binary)],check=True)
    modes=[{}]+[{'FAIL_AT':str(i),'EXPECT_FAIL':'1'} for i in range(1,12)]
    modes += [{name:'1','EXPECT_FAIL':'1'} for name in ('BAD_ALIGN','NO_IMPORT','NO_WINDOW','NO_PROC','BIG_REQ','NO_TYPE','CORRUPT','NO_PRESENT')]
    modes += [{'NO_EXT':'1'}]
    for mode in modes:
        result=subprocess.run([str(binary)],env=dict(os.environ,**mode),capture_output=True,text=True,timeout=5)
        assert result.returncode==0,(mode,result.stdout,result.stderr)
        assert ('VULKAN_SHARED_PASS' in result.stdout)==(not mode)
    modes=[{}]+[{'FAIL_AT':str(i),'EXPECT_FAIL':'1'} for i in range(1,5)]
    modes += [{name:'1','EXPECT_FAIL':'1'} for name in ('NO_DRAW','NO_PRESENT','BIG_REQ','NO_TYPE')]
    for mode in modes:
        result=subprocess.run([str(binary)],env=dict(os.environ,RENDER='1',**mode),capture_output=True,text=True,timeout=5)
        assert result.returncode==0,(mode,result.stdout,result.stderr)
        assert ('VULKAN_SHARED_PASS' in result.stdout)==(not mode)
    modes=[{}]+[{'FAIL_AT':str(i),'EXPECT_FAIL':'1'} for i in range(1,5)]
    modes += [{'NO_LIFETIME':'1','EXPECT_FAIL':'1'}]
    for mode in modes:
        result=subprocess.run([str(binary)],env=dict(os.environ,LIFETIME='1',**mode),capture_output=True,text=True,timeout=5)
        assert result.returncode==0,(mode,result.stdout,result.stderr)
        assert ('VULKAN_LIFETIME_PASS' in result.stdout)==(not mode)
print('Vulkan shared surface: actual API path, GPU fill/barrier/fence/readback, 11 API failures, alignment/capability/size/type/pixel/present rejection and GPU-before-window release passed; guest window and all Vulkan calls mocked')
print('Vulkan shared graphics handoff: import before renderer, draw before present, no fill fallback, draw/import/present failures and ownership cleanup passed')
print('Vulkan lifetime handoff: import with transfer-source capability, descriptor consumed once, pinned alias retained until GPU release, early failures and no renderer/fill fallback passed')
