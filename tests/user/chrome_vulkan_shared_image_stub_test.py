"""Execute the actual shared-image context acquisition and current binding."""
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from chromium_source import chromium_source
root = chromium_source()
source = (root / 'gpu/ipc/service/shared_image_stub.cc').read_text()
start = source.index('bool SharedImageStub::MakeContextCurrent(')
binding = source[start:source.index('ContextResult SharedImageStub::Initialize()', start)]
start = source.index('  const bool gl_enabled =', source.index('ContextResult SharedImageStub::Initialize()'))
acquisition = source[start:source.index('  factory_ =', start)]
factory = (root / 'gpu/command_buffer/service/shared_image/shared_image_factory.cc').read_text()
start = factory.index('void SharedImageFactory::LogGetFactoryFailed(')
log_body = factory[start:factory.index('bool SharedImageFactory::RegisterBacking(', start)]
assert 'context_state_ && context_state_->GrContextIsVulkan()' in log_body
assert 'context_state_ && context_state_->gr_context_type()' in log_body
guards = []
for signature in ('  if (context_state_ && context_state_->GrContextIsVulkan()',
                  '  if (context_state_ && context_state_->gr_context_type()'):
    start = log_body.index(signature)
    guards.append(log_body[start:log_body.index('\n  }', start) + len('\n  }')])

fixture = r'''
#include <cassert>
#include <initializer_list>
#include <string>
#define BUILDFLAG(x) 1
#define DCHECK(x) assert(x)
struct Log {template<class T> Log& operator<<(T const&) {return *this;}};
#define LOG(x) Log{}
enum class ContextResult {kSuccess,kFatalFailure,kTransientFailure};
enum class GrContextType {kNone,kGL,kVulkan};
enum {GPU_FEATURE_TYPE_VULKAN,kGpuFeatureStatusEnabled=1};
namespace gl {
enum {kGLImplementationDisabled,kGLImplementationEnabled};
static int implementation;
int GetGLImplementation(){return implementation;}
struct Context {bool current=false;bool IsCurrent(void*){return current;}};
}
struct State {
 GrContextType type=GrContextType::kVulkan;
 bool GrContextIsVulkan(){return type==GrContextType::kVulkan;}
 GrContextType gr_context_type(){return type;}
 gl::Context* context=nullptr;
 bool lost=false, can_bind=true;
 int calls=0;
 bool requested_gl=false;
 gl::Context* real_context(){return context;}
 bool context_lost(){return lost;}
 bool CheckResetStatus(bool){return lost;}
 bool MakeCurrent(void*,bool needs_gl){
  ++calls;requested_gl=needs_gl;
  return can_bind&&(!needs_gl||context);
 }
};
struct Manager {
 struct {GrContextType gr_context_type=GrContextType::kGL;} preferences;
 struct {int status_values[1]={0};} feature;
 State state;
 ContextResult result=ContextResult::kSuccess;
 unsigned acquired=0;
 auto& gpu_preferences(){return preferences;}
 auto& gpu_feature_info(){return feature;}
 State* GetSharedContextState(ContextResult* r){++acquired;*r=result;return &state;}
};
struct Channel {Manager manager;Manager* gpu_channel_manager(){return &manager;}};
template<class T> struct Ptr {
 T* p=nullptr;operator bool() const{return p;}
 T* operator->(){return p;}Ptr& operator=(T* v){p=v;return *this;}
};
struct SharedImageStub {
 Channel* channel_;
 Ptr<State> context_state_;
 bool MakeContextCurrent(bool);
 ContextResult Initialize(){
  auto* channel_manager=channel_->gpu_channel_manager();
  @ACQUISITION@
  return ContextResult::kSuccess;
 }
};
@BINDING@
bool log_was_completed=false;
void log_failure(State* context_state_, const std::string& new_debug_label){
 @GUARDS@
 log_was_completed=true;
}
int main(){
 log_failure(nullptr,"VizBufferQueue");assert(log_was_completed);
 log_was_completed=false;
 log_failure(nullptr,"LayerTreeHostUIResourceBitmap");assert(log_was_completed);
 State state;
 log_was_completed=false;
 log_failure(&state,"VizBufferQueue");assert(!log_was_completed);
 state.type=GrContextType::kNone;
 log_failure(&state,"LayerTreeHostUIResourceBitmap");assert(!log_was_completed);
 log_failure(&state,"UnknownResource");assert(log_was_completed);
 for(bool gl_enabled:{false,true}) for(bool vk:{false,true})
 for(bool feature:{false,true}) for(bool bind:{false,true})
 for(bool created:{false,true}) {
  Channel c;c.manager.preferences.gr_context_type=vk?GrContextType::kVulkan:GrContextType::kGL;
  c.manager.feature.status_values[0]=feature;
  c.manager.state.can_bind=bind;
  gl::Context context;c.manager.state.context=gl_enabled?&context:nullptr;
  c.manager.result=created?ContextResult::kSuccess:ContextResult::kFatalFailure;
  gl::implementation=gl_enabled;
  SharedImageStub s{&c,{}};
  bool acquire=gl_enabled;
 #ifdef __NATIVE_OS__
  acquire=acquire||(vk&&feature);
 #endif
  auto expected=!acquire?ContextResult::kSuccess:
   !created?ContextResult::kFatalFailure:
   !bind?ContextResult::kTransientFailure:ContextResult::kSuccess;
  assert(s.Initialize()==expected);
  assert(c.manager.acquired==unsigned(acquire));
  assert(bool(s.context_state_)==(acquire&&created&&bind));
  if(acquire&&created) assert(c.manager.state.requested_gl==gl_enabled);
 }
 Channel c;SharedImageStub s{&c,{}};
 assert(s.MakeContextCurrent(false));
 s.context_state_=&c.manager.state;
 assert(s.MakeContextCurrent(false));
 assert(!s.MakeContextCurrent(true));
 c.manager.state.lost=true;
 assert(!s.MakeContextCurrent(false));
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory) / 'stub.cc'
    path.write_text(fixture.replace('@ACQUISITION@', acquisition).replace('@BINDING@', binding).replace('@GUARDS@', '\n'.join(guards)))
    for native in (False, True):
        binary = Path(directory) / ('native' if native else 'upstream')
        command = ['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-fno-pie', '-no-pie']
        if native:
            command.append('-D__NATIVE_OS__=1')
        subprocess.run(command + [str(path), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
print('Shared-image stub: native/upstream context acquisition, GL-free bind, missing GL rejection and failure cleanup passed')
