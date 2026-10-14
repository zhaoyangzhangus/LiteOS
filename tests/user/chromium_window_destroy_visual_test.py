"""Exercise production Aura destruction ordering and UpdateVisualState guard.

The small fixture substitutes layer/observer plumbing only. A full Aura
WindowTest also covers the same nested frame path in Chromium's test suite.
"""
from pathlib import Path as _TestPath
import sys as _test_sys
_test_sys.path.insert(0, str(_TestPath(__file__).resolve().parents[1]))
from chromium_source import chromium_source
from pathlib import Path
import subprocess
import tempfile

source = (chromium_source() / 'ui/aura/window.cc').read_text()
start = source.index('  is_destroying_ = true;', source.index('Window::~Window()'))
end = source.index('  // While we are being destroyed', start)
destroy = source[start:end]
start = source.index('void Window::UpdateVisualState() {')
end = source.index('\nvoid Window::GetDebugInfo(', start)
update = source[start:end]
program = r'''
#include <array>
#include <cassert>
#define DUMP_WILL_BE_CHECK(value) assert(value)
class Window;
struct FakeLayer {
    Window* owner_;
    Window* delegate_;
    int frame_requests = 0;
    Window* owner() { return owner_; }
    void set_delegate(Window* value) { delegate_ = value; }
    void CompleteAllAnimations() { SendDamagedRects(); }
    void SendDamagedRects();
};
struct Delegate {
    int visual_updates = 0;
    int destroying = 0;
    void UpdateVisualState() { ++visual_updates; }
    void OnWindowDestroying(Window* window);
};
struct WindowObserver {
    int* destroying;
    void OnWindowDestroying(Window* window);
};
struct WindowOcclusionTracker {
    struct ScopedPause { ~ScopedPause() {} };
};
class Window {
public:
    explicit Window(Delegate* delegate, int* observer_count)
        : delegate_(delegate), observers_{{{observer_count}}}, layer_{this, this} {}
    FakeLayer* layer() { return &layer_; }
    void BeginDestruction() {
''' + destroy + r'''
    }
    void UpdateVisualState();
    bool is_destroying_ = false;
    Delegate* delegate_;
    std::array<WindowObserver, 1> observers_;
    FakeLayer layer_;
};
void FakeLayer::SendDamagedRects() {
    ++frame_requests;
    if (delegate_) delegate_->UpdateVisualState();
}
void Delegate::OnWindowDestroying(Window* window) {
    assert(window->is_destroying_);
    ++destroying;
    window->layer()->SendDamagedRects();
}
void WindowObserver::OnWindowDestroying(Window* window) {
    assert(window->is_destroying_);
    ++*destroying;
    window->layer()->SendDamagedRects();
}
''' + update + r'''
int main() {
    Delegate delegate;
    int observer_count = 0;
    Window window(&delegate, &observer_count);
    window.layer()->SendDamagedRects();
    assert(delegate.visual_updates == 1);
    window.BeginDestruction();
    assert(window.is_destroying_ && window.layer()->delegate_ == nullptr);
    assert(delegate.destroying == 1 && observer_count == 1);
    assert(window.layer()->frame_requests == 4);
    assert(delegate.visual_updates == 1);
    window.layer()->SendDamagedRects();
    assert(delegate.visual_updates == 1);
}
'''
with tempfile.TemporaryDirectory(prefix='system-aura-destroy-') as directory:
    path = Path(directory)
    (path / 'test.cc').write_text(program)
    subprocess.run(['c++', '-std=c++17', '-D__NATIVE_OS__', '-Wall', '-Wextra',
                    '-Werror', '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(path / 'test.cc'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('Aura destruction: nested animation/delegate/observer frames blocked; notifications preserved')
