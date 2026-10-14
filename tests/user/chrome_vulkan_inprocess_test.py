"""Execute native in-process Vulkan initialization and device-loss policy."""
from pathlib import Path
import subprocess
import sys
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from chromium_source import chromium_source
root = chromium_source()
init = (root/'gpu/ipc/service/gpu_init.cc').read_text()
begin = init.index('bool GpuInit::InitializeNativeGuestInProcessVulkan(')
policy = init[begin:init.index('bool GpuInit::InitializeVulkan()', begin)]
service = (root/'components/viz/service/gl/gpu_service_impl.cc').read_text()
begin = service.index('void GpuServiceImpl::MaybeExitOnContextLost(')
loss = service[begin:service.index('bool GpuServiceImpl::IsExiting()', begin)]
start = service.index('#if defined(__NATIVE_OS__)', service.index('if (!vulkan_context_provider_)'))
device_policy = service[start:service.index('#endif', start) + len('#endif')]
native = init[init.index('#if defined(__NATIVE_OS__) && BUILDFLAG(ENABLE_VULKAN)') :]
assert 'GetSwitchValueASCII("ozone-platform") == "nativeguest"' in native
assert 'use_vulkan == VulkanImplementationName::kForcedNative' in native
assert 'disable_vulkan_surface' in native
assert native.index('InitializeNativeGuestInProcessVulkan(gl_disabled)') < native.index('DisableInProcessGpuVulkan(')
code = r'''
#include <cassert>
#define __NATIVE_OS__ 1
#define DCHECK(x) assert(x)
enum {ERROR, WARNING, FATAL};
struct Fatal {};
struct Log {
  int severity;
  template<class T> Log& operator<<(T const&) {return *this;}
  ~Log() noexcept(false) {if(severity==FATAL) throw Fatal{};}
};
#define LOG(x) Log{x}
namespace gpu {
enum class GrContextType {kGL,kVulkan};
namespace error {enum ContextLostReason {kUnknown};}
}
using gpu::GrContextType;
enum {GPU_FEATURE_TYPE_VULKAN, GPU_FEATURE_TYPE_ACCELERATED_GL,
      GPU_FEATURE_TYPE_ACCELERATED_WEBGL, GPU_FEATURE_TYPE_ACCELERATED_WEBGL2};
enum {kGpuFeatureStatusDisabled=0, kGpuFeatureStatusEnabled=1};
enum {VK_PHYSICAL_DEVICE_TYPE_CPU=4};
struct DeviceProperties {unsigned deviceType=2,vendorID=0x10de,deviceID=0x2f04;const char* deviceName="NVIDIA GeForce RTX 5070";};
struct DeviceQueue {DeviceProperties device;DeviceProperties const& vk_physical_device_properties(){return device;}};
struct Provider {DeviceQueue queue;DeviceQueue* GetDeviceQueue(){return &queue;}};
struct Preferences {GrContextType gr_context_type=GrContextType::kVulkan; bool disable_vulkan_surface=true;};
struct Features {int status_values[4]={1,1,1,1};};
struct GpuInit {
 Preferences gpu_preferences_; Features gpu_feature_info_;
 bool init_successful_=true, driver_success=true; unsigned driver_calls=0;
 bool InitializeVulkan(){++driver_calls;return driver_success;}
 bool InitializeNativeGuestInProcessVulkan(bool);
};
struct Runner {bool BelongsToCurrentThread(){return true;}};
struct GpuServiceImpl {
 Runner runner; Runner* main_runner_=&runner;
 Preferences gpu_preferences_; bool host=true; unsigned restarts=0;
 bool in_host_process(){return host;}
 void RestartGpuProcessForContextLoss(const char*){++restarts;}
 void MaybeExitOnContextLost(gpu::error::ContextLostReason);
};
void CheckDevice(Preferences gpu_preferences_,Provider* vulkan_context_provider_){
''' + device_policy + r'''
}
''' + policy + loss + r'''
int main(){
 Provider provider;Preferences preferences;
 CheckDevice(preferences,&provider);
 try{CheckDevice(preferences,nullptr);assert(false);}catch(Fatal const&){}
 provider.queue.device.deviceType=VK_PHYSICAL_DEVICE_TYPE_CPU;
 try{CheckDevice(preferences,&provider);assert(false);}catch(Fatal const&){}
 for(unsigned type=0;type<4;++type){provider.queue.device.deviceType=type;CheckDevice(preferences,&provider);}
 preferences.disable_vulkan_surface=false;CheckDevice(preferences,nullptr);
 preferences.disable_vulkan_surface=true;preferences.gr_context_type=GrContextType::kGL;CheckDevice(preferences,nullptr);
 GpuInit ok; assert(ok.InitializeNativeGuestInProcessVulkan(true));
 assert(ok.init_successful_ && ok.driver_calls==1);
 assert(ok.gpu_feature_info_.status_values[GPU_FEATURE_TYPE_VULKAN]==kGpuFeatureStatusEnabled);
 for(unsigned type=1;type<4;++type)assert(ok.gpu_feature_info_.status_values[type]==kGpuFeatureStatusDisabled);
 for(int failure=0; failure<4; ++failure){
  GpuInit bad;
  if(failure==1)bad.gpu_preferences_.gr_context_type=GrContextType::kGL;
  if(failure==2)bad.gpu_feature_info_.status_values[0]=0;
  if(failure==3)bad.driver_success=false;
  assert(!bad.InitializeNativeGuestInProcessVulkan(failure!=0));
  assert(!bad.init_successful_ && bad.driver_calls==(failure==3?1u:0u));
 }
 GpuServiceImpl service;
 try{service.MaybeExitOnContextLost(gpu::error::kUnknown);assert(false);}catch(Fatal const&){}
 assert(service.restarts==0);
 service.gpu_preferences_.disable_vulkan_surface=false;
 service.MaybeExitOnContextLost(gpu::error::kUnknown);assert(service.restarts==0);
 service.gpu_preferences_.disable_vulkan_surface=true;
 service.gpu_preferences_.gr_context_type=GrContextType::kGL;
 service.MaybeExitOnContextLost(gpu::error::kUnknown);assert(service.restarts==0);
 service.host=false;
 service.MaybeExitOnContextLost(gpu::error::kUnknown);assert(service.restarts==1);
}
'''
with tempfile.TemporaryDirectory(prefix='chrome-vulkan-inprocess-') as directory:
    base = Path(directory)
    (base/'test.cc').write_text(code)
    subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    str(base/'test.cc'), '-o', str(base/'test')], check=True)
    subprocess.run([str(base/'test')], check=True)
print('Native Vulkan in-process: actual initialization success, GL/feature/driver '
      'failure, device identity, missing/CPU device rejection, no silent fallback, '
      'host device loss and subprocess recovery passed', flush=True)
subprocess.run([sys.executable, str(Path(__file__).with_name('chrome_vulkan_compile_test.py')),
                '--runtime', 'glibc', '--gpu-init-only'], check=True, timeout=80)
