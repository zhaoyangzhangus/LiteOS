"""Execute the display creation binding gate from real Chromium source."""
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from chromium_source import chromium_source
source = (chromium_source() /
          'components/viz/service/display_embedder/skia_output_surface_impl_on_gpu.cc').read_text()
start = source.index('  bool need_gl = true;', source.index('SkiaOutputSurfaceImplOnGpu::Create('))
gate = source[start:source.index('  auto impl_on_gpu =', start)]
fixture = r'''
#include <cassert>
#include <initializer_list>
#define BUILDFLAG(x) 1
struct Log {template<class T> Log& operator<<(T const&) {return *this;}};
#define LOG(x) Log{}
struct State {
 bool vk, provider, gl, allowed;
 unsigned calls=0;
 bool requested_gl=true, reset=false;
 bool GrContextIsVulkan() const{return vk;}
 bool vk_context_provider() const{return provider;}
 bool context() const{return gl;}
 bool MakeCurrent(void*,bool needs_gl){++calls;requested_gl=needs_gl;return allowed&&(!needs_gl||gl);}
 void set_need_context_state_reset(bool value){reset=value;}
};
State* create(State* context_state){
 @GATE@
 return context_state;
}
int main(){
 for(bool vk:{false,true}) for(bool provider:{false,true})
 for(bool gl:{false,true}) for(bool allowed:{false,true}) {
  State s{vk,provider,gl,allowed};
  bool needs_gl=true;
 #ifdef __NATIVE_OS__
  needs_gl=!(vk&&provider&&!gl);
 #endif
  bool success=allowed&&(!needs_gl||gl);
  assert((create(&s)!=nullptr)==success);
  assert(s.calls==1&&s.requested_gl==needs_gl);
  assert(s.reset==success);
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
print('Display binding: genuine Vulkan-only, missing provider, GL interop, binding failure and upstream policy passed')
