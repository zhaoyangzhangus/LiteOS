"""Exercise the actual desktop-host show policy used by notification bubbles."""
from pathlib import Path as _TestPath
import sys as _test_sys
_test_sys.path.insert(0, str(_TestPath(__file__).resolve().parents[1]))
from chromium_source import chromium_source
from pathlib import Path
import subprocess
import tempfile

source = (chromium_source() / 'ui/views/widget/desktop_aura/desktop_window_tree_host_platform.cc').read_text()
start = source.index('bool DetermineInactivity(')
end = source.index('\nvoid LogPlatformWidgetStage', start)
assert 'show_state, native_widget_delegate_->CanActivate()' in source
program = r'''
#include <cassert>
#define NOTIMPLEMENTED_LOG_ONCE() ((void)0)
namespace ui::mojom {
enum class WindowShowState {kDefault,kNormal,kInactive,kMaximized,kMinimized,kFullscreen};
}
''' + source[start:end] + r'''
int main() {
  using State=ui::mojom::WindowShowState;
  for (auto state : {State::kDefault,State::kNormal,State::kInactive,
                    State::kMaximized,State::kMinimized,State::kFullscreen}) {
    assert(DetermineInactivity(state,false));
    assert(DetermineInactivity(state,true)==(state==State::kInactive));
  }
}
'''
program = '#include <initializer_list>\n' + program
with tempfile.TemporaryDirectory(prefix='system-show-policy-') as directory:
    path = Path(directory)
    (path/'test.cc').write_text(program)
    subprocess.run(['c++','-std=c++20','-Wall','-Wextra','-Werror',
                    str(path/'test.cc'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('non-activatable normal/fullscreen notifications preserve native focus: passed')
