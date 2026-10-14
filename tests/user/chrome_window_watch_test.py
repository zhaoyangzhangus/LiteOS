"""Exercise the production window-watch lifecycle with instrumented UI/fd stubs."""
from pathlib import Path as _TestPath
import sys as _test_sys
_test_sys.path.insert(0, str(_TestPath(__file__).resolve().parents[1]))
from chromium_source import chromium_source
from pathlib import Path
import subprocess
import tempfile

source_root = chromium_source()
source = (source_root / 'ui/ozone/platform/nativeguest/nativeguest_platform_event_source.cc').read_text()
watch = source[source.index('struct NativeGuestPlatformEventSource::WindowWatch {'):
               source.index('NativeGuestPlatformEventSource::NativeGuestPlatformEventSource(')]
methods = source[source.index('void NativeGuestPlatformEventSource::WatchWindow('):
                 source.index('void NativeGuestPlatformEventSource::OnFileCanReadWithoutBlocking(')]
program = r'''
#include <cassert>
#include <cstdint>
#include <fcntl.h>
#include <map>
#include <memory>
#include <set>
#include <vector>
#include <cstdarg>
#if !TEST_GLIBC
#undef __GLIBC__
#endif
#define FROM_HERE 0
static std::vector<int> trace;
static bool ui_ready = true, register_ok = true, adopt_ok = true;
static int64_t dup_result = 7;
#if TEST_GLIBC
static bool set_flags_ok = true;
static constexpr int watch_fd = 7;
int fixture_fcntl(int fd, int cmd, ...) {
  assert(fd == 7);
  if (cmd == F_GETFL) {
    trace.push_back(1);
    return adopt_ok ? O_RDONLY : -1;
  }
  assert(cmd == F_SETFL);
  va_list arguments;
  va_start(arguments, cmd);
  assert(va_arg(arguments, int) == (O_RDONLY | O_NONBLOCK));
  va_end(arguments);
  return set_flags_ok ? 0 : -1;
}
#define fcntl fixture_fcntl
#else
static constexpr int watch_fd = 17;
#endif
namespace gfx { using AcceleratedWidget = uint64_t; }
namespace base {
inline int Milliseconds(int value) { return value; }
template<class T> T* Unretained(T* value) { return value; }
template<class A,class B> int BindRepeating(A, B) { return 0; }
struct RepeatingTimer {
  bool running = false;
  unsigned starts = 0;
  bool IsRunning() const { return running; }
  void Stop() { running = false; }
  void Start(int, int interval, int) {
    assert(interval == 8); running = true; ++starts;
  }
};
struct ScopedFD {
  int value = -1;
  void reset(int fd) { value = fd; }
  bool is_valid() const { return value >= 0; }
  int get() const { return value; }
  ~ScopedFD() { if (is_valid()) trace.push_back(4); }
};
struct MessagePumpForUI {
  enum { WATCH_READ = 1 };
  struct FdWatchController {
    bool active = false;
    explicit FdWatchController(int) {}
    ~FdWatchController() { if (active) trace.push_back(3); }
  };
};
struct CurrentUIThread {
  static bool IsSet() { return ui_ready; }
  static CurrentUIThread* Get() { static CurrentUIThread thread; return &thread; }
  bool WatchFileDescriptor(int fd, bool persistent, int mode,
      MessagePumpForUI::FdWatchController* controller, void*) {
    assert(fd == watch_fd && persistent && mode == MessagePumpForUI::WATCH_READ);
    trace.push_back(2);
    controller->active = register_ok;
    return register_ok;
  }
};
}
int __libc_adopt_linux_fd(int raw, int flags) {
  assert(raw == 7 && flags == (O_RDONLY | O_NONBLOCK | O_CLOEXEC));
  trace.push_back(1);
  return adopt_ok ? 17 : -1;
}
namespace ui {
namespace nativeguest {
int64_t RawSyscall3(uint64_t nr, uint64_t fd, uint64_t command, uint64_t min) {
  assert(nr == 72 && fd == 5 && command == F_DUPFD_CLOEXEC && min == 0);
  trace.push_back(0);
  return dup_result;
}
}
class NativeGuestPlatformEventSource {
 public:
  struct WindowWatch;
  void WatchWindow(gfx::AcceleratedWidget widget);
  void UnwatchWindow(gfx::AcceleratedWidget widget);
  void UpdateFallbackTimer();
  void Pump() {}
  base::RepeatingTimer timer_;
  std::map<gfx::AcceleratedWidget, std::unique_ptr<WindowWatch>> watches_;
  std::set<gfx::AcceleratedWidget> pending_watches_;
};
''' + watch + methods + r'''
}
int main() {
  ui::NativeGuestPlatformEventSource source;
  assert(!source.timer_.IsRunning());
  ui_ready = false;
  source.WatchWindow(5); assert(trace.empty());
  assert(source.timer_.IsRunning() && source.pending_watches_.contains(5));
  source.WatchWindow(5);
  assert(source.timer_.starts == 1); // Retry must not restart its deadline.
  ui_ready = true; dup_result = -24;
  source.WatchWindow(5); assert((trace == std::vector<int>{0}));
  trace.clear(); dup_result = 7; adopt_ok = false;
  source.WatchWindow(5);
#if TEST_GLIBC
  assert((trace == std::vector<int>{0, 1, 4}));
  trace.clear(); adopt_ok = true; set_flags_ok = false;
  source.WatchWindow(5); assert((trace == std::vector<int>{0, 1, 4}));
  set_flags_ok = true;
#else
  assert((trace == std::vector<int>{0, 1}));
#endif
  trace.clear(); adopt_ok = true; register_ok = false;
  source.WatchWindow(5); assert((trace == std::vector<int>{0, 1, 2, 4}));
  assert(source.timer_.IsRunning());
  trace.clear(); register_ok = true;
  source.WatchWindow(5); source.WatchWindow(5);
  assert((trace == std::vector<int>{0, 1, 2}));
  assert(!source.timer_.IsRunning() && source.pending_watches_.empty());
  ui_ready = false;
  source.WatchWindow(6); source.WatchWindow(8);
  assert(source.timer_.IsRunning() && source.pending_watches_.size() == 2);
  source.UnwatchWindow(6);
  assert(source.timer_.IsRunning() && source.pending_watches_.contains(8));
  source.UnwatchWindow(8);
  assert(!source.timer_.IsRunning() && source.watches_.contains(5));
  ui_ready = true;
  source.UnwatchWindow(5);
  assert((trace == std::vector<int>{0, 1, 2, 3, 4}));
  trace.clear();
  source.UnwatchWindow(5); assert(trace.empty());
  source.WatchWindow(5); source.watches_.clear();
  assert((trace == std::vector<int>{0, 1, 2, 3, 4}));
}
'''
with tempfile.TemporaryDirectory(prefix='system-window-watch-') as directory:
    path = Path(directory)
    (path / 'test.cc').write_text(program)
    for glibc in (0, 1):
        subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                        '-DTEST_GLIBC='+str(glibc), str(path / 'test.cc'),
                        '-o', str(path / 'test')], check=True)
        subprocess.run([str(path / 'test')], check=True)
print('window watch: fd namespace, retry, deduplication, stop-before-close, failure cleanup and fallback-only timer passed')
