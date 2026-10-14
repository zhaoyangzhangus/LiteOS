"""Native alpha flags and Aura transparency must agree before the first frame."""
from pathlib import Path as _TestPath
import sys as _test_sys
_test_sys.path.insert(0, str(_TestPath(__file__).resolve().parents[1]))
from chromium_source import chromium_source
from pathlib import Path
import subprocess
import tempfile

source = (chromium_source() / 'ui/ozone/platform/nativeguest/nativeguest_window.cc').read_text()
begin = source.index('  contents_transparent_ =', source.index('NativeGuestWindow::NativeGuestWindow('))
end = source.index('\n\n  int64_t result', begin)
setup = source[begin:end]
begin = source.index('bool NativeGuestWindow::ShouldWindowContentsBeTransparent() const {')
end = source.index('\n}', begin) + 2
getter = source[begin:end]
program = r'''
#include <cassert>
#include <cstdint>
enum class PlatformWindowOpacity { kInferOpacity, kOpaqueWindow, kTranslucentWindow };
namespace nativeguest { constexpr uint32_t kWindowPremultipliedAlpha = 8; }
struct Properties { PlatformWindowOpacity opacity; };
struct Create { uint32_t flags; uint32_t background; };
class NativeGuestWindow {
 public:
  bool contents_transparent_ = false;
  bool ShouldWindowContentsBeTransparent() const;
  void Configure(Properties properties, Create& create) {
''' + setup + r'''
  }
};
''' + getter + r'''
int main() {
  for (auto opacity : {PlatformWindowOpacity::kInferOpacity,
                       PlatformWindowOpacity::kOpaqueWindow,
                       PlatformWindowOpacity::kTranslucentWindow}) {
    for (uint32_t background : {0xffd3e3fdu, 0xff202124u}) {
      NativeGuestWindow window;
      Create create{6, background};
      assert(!window.ShouldWindowContentsBeTransparent());
      window.Configure({opacity}, create);
      bool transparent = opacity == PlatformWindowOpacity::kTranslucentWindow;
      assert(window.ShouldWindowContentsBeTransparent() == transparent);
      assert(bool(create.flags & nativeguest::kWindowPremultipliedAlpha) == transparent);
      assert((create.flags & 6) == 6);
      assert(create.background == (transparent ? 0 : background));
    }
  }
}
'''
program = '#include <initializer_list>\n' + program
with tempfile.TemporaryDirectory(prefix='system-window-transparency-') as directory:
    path = Path(directory)
    (path / 'test.cc').write_text(program)
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    str(path / 'test.cc'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('Window transparency: 6 opacity/background combinations; native and Aura states agree')
