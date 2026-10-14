"""Execute the real command-buffer GL-disabled gate for all profile kinds."""
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from chromium_source import chromium_source

source = (chromium_source() /
          'services/viz/public/cpp/gpu/context_provider_command_buffer.cc').read_text()
start = source.index('  bool allow_vulkan_raster = false;')
gate = source[start:source.index('  // Any early-out', start)]
fixture = r'''
#include <cassert>
#include <string>
namespace gpu {
enum {GPU_FEATURE_TYPE_VULKAN, kGpuFeatureStatusEnabled=1};
enum class ContextResult {kSuccess, kFatalFailure};
namespace mojom {
struct ContextCreationAttribs {
 enum class Tag {kRaster, kGles, kWebgpu};
 Tag tag;
 Tag which() const {return tag;}
};
}
}
namespace gl {enum {kGLImplementationDisabled, kGLImplementationEnabled};}
namespace base {
struct CommandLine {
 std::string platform, mode;
 static CommandLine* ForCurrentProcess() {static CommandLine c;return &c;}
 std::string GetSwitchValueASCII(const char* name) const {
  return std::string(name)=="ozone-platform"?platform:mode;
 }
};
}
struct Feature {int status_values[1];};
struct Info {struct {int gl;} gl_implementation_parts;};
struct Channel {
 Feature feature;
 Info info;
 const Feature& gpu_feature_info() const {return feature;}
 const Info& gpu_info() const {return info;}
};
gpu::ContextResult bind(Channel* channel_, gpu::mojom::ContextCreationAttribs* attributes_) {
 (void)attributes_;
 auto bind_result_=gpu::ContextResult::kSuccess;
 @GATE@
 return bind_result_;
}
int main() {
 for(int tag=0;tag<3;++tag) for(bool vk:{false,true})
 for(bool enabled_gl:{false,true}) for(bool native_platform:{false,true})
 for(const char* mode:{"native","swiftshader",""}) {
  auto* command=base::CommandLine::ForCurrentProcess();
  command->platform=native_platform?"nativeguest":"x11";
  command->mode=mode;
  Channel channel{{{vk}},{{enabled_gl}}};
  gpu::mojom::ContextCreationAttribs attributes{
   static_cast<gpu::mojom::ContextCreationAttribs::Tag>(tag)};
  bool expected=enabled_gl;
 #ifdef __NATIVE_OS__
  expected=expected||(tag==0&&vk&&native_platform&&std::string(mode)=="native");
 #endif
  assert((bind(&channel,&attributes)==gpu::ContextResult::kSuccess)==expected);
 }
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory) / 'policy.cc'
    path.write_text(fixture.replace('@GATE@', gate))
    for native in (False, True):
        binary = Path(directory) / ('native' if native else 'upstream')
        command = ['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror']
        if native:
            command.append('-D__NATIVE_OS__=1')
        subprocess.run(command + [str(path), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
print('Raster GL-disabled gate: native/upstream, GLES/WebGPU, Vulkan status and profile matrix passed')
