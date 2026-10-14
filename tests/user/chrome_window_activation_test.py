"""Check the real LiteOS Show method's activation and failure semantics."""
from pathlib import Path as _TestPath
import sys as _test_sys
_test_sys.path.insert(0, str(_TestPath(__file__).resolve().parents[1]))
from chromium_source import chromium_source
from pathlib import Path
import subprocess
import tempfile

source = (chromium_source() / 'ui/ozone/platform/nativeguest/nativeguest_window.cc').read_text()
start = source.index('void NativeGuestWindow::Show(bool inactive)')
end = source.index('\nvoid NativeGuestWindow::Hide()', start)
activate_start = source.index('void NativeGuestWindow::Activate()')
activate_end = source.index('\nvoid NativeGuestWindow::Deactivate()',activate_start)
program = r'''
#include <cassert>
#include <cstddef>
static int write(int, const void*, size_t) { return 0; }
namespace nativeguest {
enum { kVisibility = 1 };
constexpr unsigned kVisibilityInactive = 1;
struct WindowCommand { int operation = 0, width = 0, height = 0; };
}
struct Delegate {
  int activations = 0;
  int lost_capture = 0;
  void OnLostCapture() { ++lost_capture; }
  void OnActivationChanged(bool active) { assert(active); ++activations; }
};
enum class PlatformWindowState {kNormal,kMinimized};
class NativeGuestWindow {
 public:
  bool closed_ = false, visible_ = false, valid = true, commit_ok = true;
  bool capture_pending_ = false, has_capture_ = false, capture_ok = true;
  int capture_attempts = 0;
  void SetCapture() { ++capture_attempts; has_capture_ = capture_ok; }
  PlatformWindowState window_state_ = PlatformWindowState::kNormal;
  struct Factory { bool GetWeakPtr(){return true;} } weak_ptr_factory_;
  int restores = 0;
  void Restore(){++restores;if(commit_ok)window_state_=PlatformWindowState::kNormal;}
  int commits = 0;
  int last_inactive = -1;
  Delegate delegate, *delegate_ = &delegate;
  bool IsValid() const { return valid; }
  bool Commit(const nativeguest::WindowCommand& c) {
    assert(c.operation == nativeguest::kVisibility && c.width == 1);
    last_inactive = c.height;
    ++commits; return commit_ok;
  }
  void Show(bool inactive);
  void Activate();
};
''' + source[start:end] + source[activate_start:activate_end] + r'''
int main() {
  NativeGuestWindow active;
  active.Show(false);
  assert(active.visible_ && active.commits == 1 && active.delegate.activations == 1);
  assert(active.last_inactive == 0);
  assert(active.capture_attempts == 0 && active.delegate.lost_capture == 0);
  NativeGuestWindow pending;
  pending.capture_pending_ = true;
  pending.Show(false);
  assert(pending.capture_attempts == 1 && pending.has_capture_ && !pending.capture_pending_);
  assert(pending.delegate.activations == 1 && pending.delegate.lost_capture == 0);
  NativeGuestWindow capture_failed;
  capture_failed.capture_pending_ = true; capture_failed.capture_ok = false;
  capture_failed.Show(false);
  assert(capture_failed.visible_ && !capture_failed.has_capture_ && !capture_failed.capture_pending_);
  assert(capture_failed.capture_attempts == 1 && capture_failed.delegate.lost_capture == 1);
  assert(capture_failed.delegate.activations == 1);
  NativeGuestWindow inactive;
  inactive.Show(true);
  assert(inactive.visible_ && inactive.delegate.activations == 0);
  assert(inactive.last_inactive == 1);
  inactive.Activate();
  assert(inactive.last_inactive == 0 && inactive.commits == 2 &&
         inactive.delegate.activations == 1);
  inactive.window_state_=PlatformWindowState::kMinimized;
  inactive.Activate();
  assert(inactive.restores==1 && inactive.window_state_==PlatformWindowState::kNormal);
  NativeGuestWindow failed;
  failed.commit_ok = false;
  failed.Show(false);
  assert(!failed.visible_ && failed.delegate.activations == 0);
  failed.Activate();
  assert(!failed.visible_ && failed.delegate.activations == 0);
  failed.window_state_=PlatformWindowState::kMinimized;
  const int commits=failed.commits;failed.Activate();
  assert(failed.restores==1 && failed.commits==commits && failed.delegate.activations==0);
  NativeGuestWindow invalid;
  invalid.valid = false;
  invalid.Show(false);
  assert(!invalid.visible_ && invalid.commits == 0 && invalid.delegate.activations == 0);
  invalid.Activate();
  assert(!invalid.visible_ && invalid.commits == 0 && invalid.delegate.activations == 0);
  invalid.valid = true; invalid.closed_ = true;
  invalid.Show(false);
  invalid.Activate();
  assert(invalid.commits == 0 && invalid.delegate.activations == 0);
}
'''
with tempfile.TemporaryDirectory(prefix='system-window-activation-') as directory:
    path = Path(directory)
    (path / 'test.cc').write_text(program)
    subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    str(path / 'test.cc'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('window activation: active/inactive show, invalid/closed window and commit failure passed')
