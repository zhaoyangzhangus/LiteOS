"""Compile the production capture selector and mouse-event routing block."""
from pathlib import Path as _TestPath
import sys as _test_sys
_test_sys.path.insert(0, str(_TestPath(__file__).resolve().parents[1]))
from chromium_source import chromium_source
from pathlib import Path
import subprocess
import tempfile
src=(chromium_source() / 'ui/ozone/platform/nativeguest')
manager=(src/'nativeguest_window_manager.cc').read_text()
start=manager.index('NativeGuestWindow* NativeGuestWindowManager::GetCapturedWindow()')
selector=manager[start:manager.index('\nvoid NativeGuestWindowManager::SetCursorScreenPoint',start)]
pump=(src/'nativeguest_platform_event_source.cc').read_text()
start=pump.index('        NativeGuestWindow* dispatch_window = window;')
end=pump.index('\n        DispatchEvent(event.get());',start)
route=pump[start:end].replace('continue;', 'return nullptr;')
code=r'''
#include <cassert>
#include <memory>
#include <map>
namespace gfx {
struct Point { int x=0,y=0;void Offset(int a,int b) {x+=a;y+=b;} };
struct Rect { int left,top;int x()const{return left;}int y()const{return top;} };
}
struct NativeGuestWindow;
struct Event {
 bool mouse;int flags_;gfx::Point location;NativeGuestWindow *target=nullptr;
 bool IsMouseEvent()const{return mouse;}int flags()const{return flags_;}
 void SetFlags(int f){flags_=f;}
 struct DispatcherApi {Event *event;explicit DispatcherApi(Event *e):event(e){}
 void set_target(NativeGuestWindow *w){event->target=w;} };
};
struct Native { bool mouse;int flags; };
struct NativeGuestWindowManager {
 std::map<int,std::shared_ptr<NativeGuestWindow>> windows_;gfx::Point cursor;
 NativeGuestWindow *GetCapturedWindow()const;
 gfx::Point GetCursorScreenPoint()const{return cursor;}
};
struct NativeGuestWindow {
 NativeGuestWindowManager *manager;gfx::Rect bounds;bool visible=true,captured=false;
 int events=0,flags=0;
 bool IsVisible()const{return visible;}bool HasCapture()const{return captured;}
 gfx::Rect GetBoundsInPixels()const{return bounds;}
 std::unique_ptr<Event> CreateEvent(Native n,gfx::Point p) {
  ++events;flags=n.flags;manager->cursor=p;manager->cursor.Offset(bounds.x(),bounds.y());
  return std::make_unique<Event>(Event{n.mouse,flags,p});
 }
};
''' + selector + r'''
static std::unique_ptr<Event> dispatch(NativeGuestWindowManager *manager_,NativeGuestWindow *window,
                                     Native native,gfx::Point location) {
 struct {Native input;} native_event{native};
 auto event=window->CreateEvent(native,location);
''' + route + r'''
 return event;
}
int main() {
 NativeGuestWindowManager manager;
 auto root=std::make_shared<NativeGuestWindow>(NativeGuestWindow{&manager,{10,10}});
 auto popup=std::make_shared<NativeGuestWindow>(NativeGuestWindow{&manager,{200,100}});
 manager.windows_[1]=root;manager.windows_[2]=popup;
 auto event=dispatch(&manager,root.get(),{true,17},{20,30});
 assert(event->target==root.get() && event->location.x==20);
 popup->captured=true;
 for(int flags:{17,16,20,0}) {
  event=dispatch(&manager,root.get(),{true,flags},{20,30});
  assert(event->target==popup.get() && event->flags()==flags);
  assert(event->location.x==-170 && event->location.y==-60);
  assert(manager.cursor.x==30 && manager.cursor.y==40 && root->flags==flags);
 }
 event=dispatch(&manager,root.get(),{false,8},{20,30});assert(event->target==root.get());
 event=dispatch(&manager,popup.get(),{true,0},{7,8});assert(event->target==popup.get() && event->location.x==7);
 popup->visible=false;assert(!manager.GetCapturedWindow());
 event=dispatch(&manager,root.get(),{true,0},{20,30});assert(event->target==root.get());
 popup->visible=true;popup->captured=false;assert(!manager.GetCapturedWindow());
 manager.windows_.erase(2);assert(!manager.GetCapturedWindow());
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-capture-route-') as directory:
 p=Path(directory);(p/'test.cc').write_text(code)
 subprocess.run(['c++','-std=c++20','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                 '-fno-pie','-no-pie',str(p/'test.cc'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('Capture route: origin state, negative local coordinates, modifiers, release, keyboard, same-window, hidden/released/removed capture passed')
