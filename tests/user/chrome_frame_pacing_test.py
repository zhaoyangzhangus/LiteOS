"""Run the actual native OS frame-deadline calculation against timing boundaries."""
from pathlib import Path as _TestPath
import sys as _test_sys
_test_sys.path.insert(0, str(_TestPath(__file__).resolve().parents[1]))
from chromium_source import chromium_source
from pathlib import Path
import subprocess
import tempfile

source = (chromium_source() / 'ui/ozone/platform/nativeguest/nativeguest_window.cc').read_text()
begin = source.index('base::TimeDelta NativeGuestNextFrameDelay(')
end = source.index('class NativeGuestBeginFrameSource final', begin)
reset_begin = source.index('  void Reset() override {', end)
reset_end = source.index('  void SetDelegate(', reset_begin)
program = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
namespace base {
struct TimeDelta {
  int64_t value = 0;
  bool operator<(TimeDelta other) const { return value < other.value; }
};
struct TimeTicks {
  int64_t value = 0;
  bool is_null() const { return value == 0; }
  TimeTicks operator+(TimeDelta delta) const { return {value + delta.value}; }
  TimeDelta operator-(TimeTicks other) const { return {value - other.value}; }
};
}
''' + source[begin:end] + r'''
struct ResetBase { virtual void Reset() = 0; };
struct ResetFixture : ResetBase {
  struct Timer { bool running = true; void Stop() { running = false; } } timer_;
  struct WeakFactory {
    unsigned generation = 1;
    void InvalidateWeakPtrs() { ++generation; }
  } weak_ptr_factory_;
  bool frame_in_flight_ = true;
  base::TimeTicks last_frame_time_{100};
''' + source[reset_begin:reset_end] + r'''
};
int main() {
  assert(NativeGuestNextFrameDelay({100}, {100}, {16}).value == 16);
  assert(NativeGuestNextFrameDelay({100}, {101}, {16}).value == 15);
  assert(NativeGuestNextFrameDelay({100}, {115}, {16}).value == 1);
  assert(NativeGuestNextFrameDelay({100}, {116}, {16}).value == 0);
  assert(NativeGuestNextFrameDelay({100}, {130}, {16}).value == 0);
  assert(NativeGuestNextFrameDelay({}, {130}, {16}).value == 16);
  assert(NativeGuestNextFrameDelay({100}, {105}, {8}).value == 3);
  ResetFixture source;
  const auto old_ack_generation = source.weak_ptr_factory_.generation;
  source.Reset();
  assert(!source.timer_.running && !source.frame_in_flight_);
  assert(source.last_frame_time_.is_null());
  assert(source.weak_ptr_factory_.generation != old_ack_generation);
}
'''
with tempfile.TemporaryDirectory(prefix='system-frame-pacing-') as directory:
    path = Path(directory)
    (path / 'test.cc').write_text(program)
    subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    str(path / 'test.cc'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('frame pacing: early/on-time/late ACK, reset and interval changes passed')
