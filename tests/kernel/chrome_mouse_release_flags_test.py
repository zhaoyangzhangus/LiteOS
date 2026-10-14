"""Exercise the actual LiteOS Ozone button-state conversion with C++ stubs."""
from pathlib import Path as _TestPath
import sys as _test_sys
_test_sys.path.insert(0, str(_TestPath(__file__).resolve().parents[1]))
from chromium_source import chromium_source
import pathlib
import subprocess
import tempfile

source = (chromium_source() / 'ui/ozone/platform/nativeguest/nativeguest_window.cc').read_text()
start = source.index('    const bool pressed = input.value != nativeguest::kRelease;')
end = source.index('    event->set_source_device_id', start)
block = source[start:end]
program = '''
#include <cassert>
#include <memory>
#include <initializer_list>
namespace nativeguest { constexpr int kRelease = 0; }
enum class EventType { kMousePressed, kMouseReleased };
struct MouseEvent {
 int flags, changed;
 MouseEvent(EventType, int, int, int, int f, int c):flags(f),changed(c){}
};
int main() {
 int keyboard_modifier_flags_ = 4, mouse_button_flags_ = 0;
 int& buttons = mouse_button_flags_;
 int location = 0, timestamp = 0, button = 256;
 struct { int value; } input;
 for (int held : {0, 512}) {
 mouse_button_flags_ = held;
 for (int value : {1, 0}) {
  input.value = value;
''' + block + '''
  assert((event->flags & button) != 0);
  assert((event->flags & 4) != 0);
  assert(event->changed == button);
  assert(mouse_button_flags_ == (held | (value ? button : 0)));
  assert((event->flags & held) == held);
 }
 assert(mouse_button_flags_ == held);
 }
}
'''
with tempfile.TemporaryDirectory() as directory:
    cpp = pathlib.Path(directory) / 'test.cc'
    cpp.write_text(program)
    exe = pathlib.Path(directory) / 'test'
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('Mouse press/release: trigger flags, modifiers and persistent state passed')
