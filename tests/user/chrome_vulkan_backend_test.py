"""Execute native Vulkan policy with deterministic loader and queue fixtures."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from chromium_source import chromium_source
root = chromium_source()/'ui/ozone/platform/nativeguest'
header = (root/'nativeguest_vulkan_implementation.h').read_text()
source = (root/'nativeguest_vulkan_implementation.cc').read_text()
header = re.sub(r'^#include.*$', '', header, flags=re.M)
source = re.sub(r'^#include.*$', '', source, flags=re.M)
factory = (root/'nativeguest_surface_factory.cc').read_text()
start = factory.index('std::unique_ptr<gpu::VulkanImplementation>')
factory_body = factory[start:factory.index('#endif',start)]
code = r'''
#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#define override
#define VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME "memory-capabilities"
#define VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME "semaphore-capabilities"
#define VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME "memory"
#define VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME "memory-fd"
#define VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME "semaphore"
#define VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME "semaphore-fd"
enum {VK_NULL_HANDLE=0,VK_QUEUE_GRAPHICS_BIT=1,VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT=1};
using VkDevice=int;using VkPhysicalDevice=int;using VkFence=int;using VkFormat=int;
using VkExternalSemaphoreHandleTypeFlagBits=int;
struct VkQueueFamilyProperties {unsigned queueFlags,queueCount;};
namespace base {struct FilePath {std::string path;FilePath(const char* s):path(s){}};}
namespace gfx {using AcceleratedWidget=unsigned;enum class GpuMemoryBufferType{SHARED_MEMORY,NATIVE_PIXMAP};struct GpuFence{};struct GpuMemoryBufferHandle{};struct Size{};struct ColorSpace{};}
namespace gpu {
struct VulkanSurface{};struct VulkanImage{};struct VulkanDeviceQueue{};
struct VulkanImplementation {VulkanImplementation(bool software,bool protected_memory,bool native){assert(!software&&!protected_memory&&native);}virtual ~VulkanImplementation()=default;};
struct VulkanInstance {
 unsigned calls=0;bool allowed=true;
 bool Initialize(base::FilePath path,std::vector<const char*> required,std::vector<const char*> optional){
  ++calls;assert(path.path=="libvulkan.so.1"&&required.size()==2&&optional.empty());
  assert(std::string(required[0])=="memory-capabilities"&&std::string(required[1])=="semaphore-capabilities");return allowed;
 }
};
}
''' + header + source + r'''
namespace ui {struct NativeGuestSurfaceFactory {
 std::unique_ptr<gpu::VulkanImplementation> CreateVulkanImplementation(bool,bool);
};
''' + factory_body + r'''
}
int main(){
 ui::NativeGuestSurfaceFactory f;
 assert(!f.CreateVulkanImplementation(true,false));assert(!f.CreateVulkanImplementation(false,true));
 assert(f.CreateVulkanImplementation(false,false));
 ui::NativeGuestVulkanImplementation v;auto* instance=v.GetVulkanInstance();
 assert(!v.InitializeVulkanInstance(true)&&!instance->calls);
 assert(v.InitializeVulkanInstance(false)&&instance->calls==1);
 instance->allowed=false;assert(!v.InitializeVulkanInstance(false)&&instance->calls==2);
 for(unsigned flags=0;flags<16;flags++)for(unsigned count=0;count<3;count++){
  std::vector<VkQueueFamilyProperties> families={{flags,count}};
  assert(v.GetPhysicalDevicePresentationSupport(0,families,0)==(count!=0&&(flags&1)!=0));
  assert(!v.GetPhysicalDevicePresentationSupport(0,families,1));
  assert(!v.GetPhysicalDevicePresentationSupport(0,families,UINT32_MAX));
 }
 assert(!v.GetPhysicalDevicePresentationSupport(0,{},0));
 assert(v.GetRequiredDeviceExtensions().empty()&&v.GetOptionalDeviceExtensions().size()==4);
 assert(!v.CreateViewSurface(7)&&!v.CreateVkFenceForGpuFence(0)&&!v.ExportVkFenceToGpuFence(0,0));
 assert(v.GetExternalSemaphoreHandleType()==VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT);
 assert(!v.CanImportGpuMemoryBuffer(nullptr,gfx::GpuMemoryBufferType::SHARED_MEMORY));
 assert(!v.CanImportGpuMemoryBuffer(nullptr,gfx::GpuMemoryBufferType::NATIVE_PIXMAP));
 assert(!v.CreateImageFromGpuMemoryHandle(nullptr,{}, {},0,{}));
}
'''
with tempfile.TemporaryDirectory(prefix='chrome-vulkan-backend-') as directory:
    base=Path(directory);(base/'test.cc').write_text(code)
    subprocess.run(['c++','-std=c++20','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    '-fno-pie','-no-pie',str(base/'test.cc'),'-o',str(base/'test')],check=True)
    subprocess.run([str(base/'test')],check=True)
print('Native Vulkan backend: hardware-only factory, loader failure, no phantom WSI, graphics queues and unsupported import guards passed')
