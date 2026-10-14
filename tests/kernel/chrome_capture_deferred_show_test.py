"""Run production popup Show/capture ordering with failed native operations."""
from pathlib import Path as _TestPath
import sys as _test_sys
_test_sys.path.insert(0, str(_TestPath(__file__).resolve().parents[1]))
from chromium_source import chromium_source
from pathlib import Path
import subprocess
import tempfile

source = (chromium_source() / 'ui/ozone/platform/nativeguest/nativeguest_window.cc').read_text()
show = source[source.index('void NativeGuestWindow::Show('):source.index('void NativeGuestWindow::Hide(')]
capture = source[source.index('void NativeGuestWindow::SetCapture()'):source.index('void NativeGuestWindow::SetFullscreen(')]
program = r'''
#include <cassert>
#include <unistd.h>
namespace nativeguest {
enum { kVisibility=1, kCapture=7, kVisibilityInactive=1 };
struct WindowCommand { int operation=0, width=0, height=0; };
}
struct Weak { bool *alive; explicit operator bool() const { return *alive; } };
struct Factory { bool *alive; Weak GetWeakPtr() { return {alive}; } };
struct Delegate {
 int lost=0, activated=0; bool *destroy=nullptr;
 void OnLostCapture() { ++lost; if(destroy)*destroy=false; }
 void OnActivationChanged(bool active) { assert(active); ++activated; }
};
struct NativeGuestWindow {
 Delegate *delegate_; bool valid=true, closed_=false, visible_=false;
 bool has_capture_=false, capture_pending_=false, alive=true;
 Factory weak_ptr_factory_{&alive};
 bool visibility_ok=true, capture_ok=true;
 int visibility_commits=0, capture_commits=0, native_capture=0;
 bool IsValid() const { return valid; }
 bool Commit(nativeguest::WindowCommand c) {
  assert(alive);
  if(c.operation==nativeguest::kVisibility) { ++visibility_commits;return visibility_ok; }
  assert(c.operation==nativeguest::kCapture);++capture_commits;
  assert(visible_);
  if(!capture_ok)return false;
  native_capture=c.width;return true;
 }
 void Show(bool);void SetCapture();void ReleaseCapture();bool HasCapture() const;
};
''' + show + capture + r'''
int main() {
 Delegate d;NativeGuestWindow w{&d};
 w.SetCapture();w.SetCapture();
 assert(w.HasCapture() && w.capture_pending_ && !w.has_capture_ && !w.capture_commits);
 w.Show(true);
 assert(w.visible_ && w.has_capture_ && !w.capture_pending_ && w.native_capture==1);
 assert(w.capture_commits==1 && d.lost==0 && d.activated==0);
 w.Show(true);assert(w.capture_commits==1);
 w.ReleaseCapture();assert(!w.HasCapture() && !w.native_capture && d.lost==1);
 Delegate cancelled;NativeGuestWindow c{&cancelled};
 c.SetCapture();c.ReleaseCapture();c.Show(true);
 assert(!c.HasCapture() && !c.capture_commits && cancelled.lost==1);
 Delegate blocked;NativeGuestWindow b{&blocked};b.visibility_ok=false;
 b.SetCapture();b.Show(true);
 assert(b.capture_pending_ && !b.visible_ && !b.capture_commits && blocked.lost==0);
 b.visibility_ok=true;b.Show(false);
 assert(b.has_capture_ && blocked.activated==1);
 Delegate failed;NativeGuestWindow f{&failed};f.capture_ok=false;
 f.SetCapture();f.Show(false);
 assert(!f.HasCapture() && failed.lost==1 && failed.activated==1);
 f.ReleaseCapture();assert(failed.lost==1);
 Delegate deleted;NativeGuestWindow gone{&deleted};gone.capture_ok=false;deleted.destroy=&gone.alive;
 gone.SetCapture();gone.Show(false);
 assert(!gone.alive && deleted.lost==1 && deleted.activated==0);
 Delegate invalid;NativeGuestWindow i{&invalid};i.valid=false;i.SetCapture();i.Show(true);
 assert(!i.HasCapture() && !i.capture_commits && !i.visibility_commits);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-capture-show-') as directory:
    path = Path(directory)
    (path/'test.cc').write_text(program)
    subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(path/'test.cc'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True)
print('Deferred menu capture: hidden/show, cancellation, native failures and callback destruction passed')
