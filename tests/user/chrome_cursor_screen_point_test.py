"""Compile the production cursor storage, coordinate conversion and screen query."""
from pathlib import Path as _TestPath
import sys as _test_sys
_test_sys.path.insert(0, str(_TestPath(__file__).resolve().parents[1]))
from chromium_source import chromium_source
from pathlib import Path
import subprocess
import tempfile

source_root = (chromium_source() / 'ui/ozone/platform/nativeguest')
manager = (source_root/'nativeguest_window_manager.cc').read_text()
start = manager.index('void NativeGuestWindowManager::SetCursorScreenPoint(')
end = manager.index('gfx::AcceleratedWidget NativeGuestWindowManager::GetAcceleratedWidgetAtScreenPoint(',start)
storage = manager[start:end]
window = (source_root/'nativeguest_window.cc').read_text()
start = window.index('std::unique_ptr<Event> NativeGuestWindow::CreateEvent(')
start = window.index('  if (manager_) {',start)
end = window.index('  const base::TimeTicks timestamp',start)
conversion = window[start:end]
assert start < window.index('  if (input.type == nativeguest::kKey)',end)
screen = (source_root/'nativeguest_screen.cc').read_text()
start = screen.index('gfx::Point NativeGuestScreen::GetCursorScreenPoint() const')
end = screen.index('gfx::AcceleratedWidget NativeGuestScreen::GetAcceleratedWidgetAtScreenPoint(',start)
query = screen[start:end]
program = r'''
#include <cassert>
namespace gfx {
struct Point { int x_=0,y_=0; void Offset(int x,int y) {x_+=x;y_+=y;} };
struct Rect { int x_=0,y_=0; int x()const{return x_;} int y()const{return y_;} };
}
struct NativeGuestWindowManager {
 gfx::Point cursor_screen_point_;
 void SetCursorScreenPoint(const gfx::Point&);
 gfx::Point GetCursorScreenPoint()const;
};
struct NativeGuestScreen {
 NativeGuestWindowManager* window_manager_;
 gfx::Point GetCursorScreenPoint()const;
};
struct NativeGuestWindow {
 NativeGuestWindowManager* manager_;
 gfx::Rect bounds_;
 void ObserveNativePoint(const gfx::Point& location) {
''' + conversion + r'''
 }
};
''' + storage + query + r'''
int main() {
 NativeGuestWindowManager manager;
 NativeGuestScreen screen{&manager},missing{nullptr};
 NativeGuestWindow browser{&manager,{10,10}},popup{&manager,{557,90}},unmanaged{nullptr,{10,10}};
 browser.ObserveNativePoint({390,62});
 auto point=screen.GetCursorScreenPoint();assert(point.x_==400 && point.y_==72);
 popup.ObserveNativePoint({93,26});
 point=screen.GetCursorScreenPoint();assert(point.x_==650 && point.y_==116);
 browser.ObserveNativePoint({-5,-3});
 point=screen.GetCursorScreenPoint();assert(point.x_==5 && point.y_==7);
 unmanaged.ObserveNativePoint({100,100});
 point=screen.GetCursorScreenPoint();assert(point.x_==5 && point.y_==7);
 point=missing.GetCursorScreenPoint();assert(point.x_==0 && point.y_==0);
}
'''
with tempfile.TemporaryDirectory(prefix='system-chrome-cursor-') as directory:
    path = Path(directory)
    (path/'test.cc').write_text(program)
    subprocess.run(['c++','-std=c++20','-Wall','-Wextra','-Werror',
                    str(path/'test.cc'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('Cursor screen point: browser/popup origins, captured negatives and null manager passed')
