"""A Vulkan context loss must not query the absent GL implementation."""
from pathlib import Path
import subprocess
import sys
import tempfile
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from chromium_source import chromium_source

source=(chromium_source()/'gpu/ipc/service/gpu_channel_manager.cc').read_text()
begin=source.index('  // Lose all other contexts.',source.index('void GpuChannelManager::OnContextLost('))
branch=source[begin:source.index('  // Work around issues with recovery',begin)]
code=r'''
#include <cassert>
namespace gl {
static unsigned queries;
static bool lose_all;
struct GLContext {static bool LosesAllContextsOnContextLost(){++queries;return lose_all;}};
}
struct State {bool virtualized;bool use_virtualized_gl_contexts(){return virtualized;}};
struct Delegate {unsigned lost=0;void LoseAllContexts(){++lost;}};
static void lose(bool is_gl,State* shared_context_state_,Delegate* delegate_){
''' + branch + r'''
}
int main(){
 for(unsigned mask=0;mask<16;++mask){
  bool is_gl=mask&1,present=mask&2,virtualized=mask&4;
  gl::lose_all=mask&8;State state{virtualized};Delegate delegate;
  unsigned before=gl::queries;
  lose(is_gl,present?&state:nullptr,&delegate);
  assert(gl::queries==before+unsigned(is_gl));
  assert(delegate.lost==unsigned(is_gl&&(gl::lose_all||(present&&virtualized))));
 }
}
'''
with tempfile.TemporaryDirectory(prefix='chrome-vulkan-loss-') as directory:
    base=Path(directory)
    (base/'test.cc').write_text(code)
    subprocess.run(['c++','-std=c++20','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    str(base/'test.cc'),'-o',str(base/'test')],check=True)
    subprocess.run([str(base/'test')],check=True)
print('Context loss: Vulkan never queries missing GL; real/virtual GL loss fanout retained across 16 combinations')
