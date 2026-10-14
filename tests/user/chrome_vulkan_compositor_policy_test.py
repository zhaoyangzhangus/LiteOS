"""Execute Chromium's real native compositor selection and raster feature gate."""
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from chromium_source import chromium_source

source = (chromium_source() / 'content/browser/compositor/viz_process_transport_factory.cc').read_text()
start = source.index('  const bool native_vulkan =')
selection = source[start:source.index('#endif', start)]
start = source.index('  auto gpu_compositing_status =')
gate = source[start:source.index('  if (worker_context_provider_', start)]
fixture = r'''
#include <cassert>
#include <string>
namespace gpu {
enum {GPU_FEATURE_TYPE_ACCELERATED_GL, GPU_FEATURE_TYPE_VULKAN};
enum {kGpuFeatureStatusDisabled, kGpuFeatureStatusEnabled};
enum class ContextResult {kSuccess, kFatalFailure};
}
namespace base {
struct CommandLine {
 std::string mode;
 static CommandLine* ForCurrentProcess() {static CommandLine c;return &c;}
 std::string GetSwitchValueASCII(const char*) {return mode;}
};
}
struct FeatureInfo {int status_values[2];};
struct Channel {FeatureInfo info;const FeatureInfo& gpu_feature_info() {return info;}};
bool select(bool gpu_compositing, Channel* gpu_channel_host) {
@SELECTION@
 return gpu_compositing;
}
gpu::ContextResult raster(const FeatureInfo& gpu_feature_info) {
@GATE@
 return gpu::ContextResult::kSuccess;
}
int main() {
 for(bool requested:{false,true}) for(bool channel:{false,true})
 for(bool vk:{false,true}) for(bool gl:{false,true}) for(bool native:{false,true}) {
  base::CommandLine::ForCurrentProcess()->mode=native?"native":"";
  Channel c{{{gl,vk}}};
  assert(select(requested,channel?&c:nullptr)==(requested&&channel&&vk&&native));
  assert((raster(c.info)==gpu::ContextResult::kSuccess)==(native?vk:gl));
 }
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory) / 'policy.cc'
    path.write_text(fixture.replace('@SELECTION@', selection).replace('@GATE@', gate))
    binary = Path(directory) / 'policy'
    subprocess.run(['c++', '-std=c++20', '-D__NATIVE_OS__=1', '-Wall', '-Wextra',
                    '-Werror', str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('Native compositor: real Vulkan/GL gates, disabled policy, missing channel and software profile matrix passed')
