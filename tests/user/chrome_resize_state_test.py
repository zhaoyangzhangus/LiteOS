"""Execute production resize handler with geometry/state callbacks."""
from pathlib import Path
import sys
import subprocess
import tempfile
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from chromium_source import chromium_source
source=(chromium_source()/'ui/ozone/platform/nativeguest/nativeguest_window.cc').read_text()
begin=source.index('void NativeGuestWindow::HandleResize(')
body=source[source.index('void NativeGuestWindow::HandleMoved('):source.index('void NativeGuestWindow::HandleCloseRequest(',begin)]
start=source.index('void NativeGuestWindow::HandleMinimized(')
body+=source[start:source.index('void NativeGuestWindow::Restore(',start)]
code=r'''
#include <cassert>
#include <cstdint>
#include <atomic>
#include <optional>
#include <memory>
#include <mutex>
namespace base {using Lock=std::mutex;using AutoLock=std::lock_guard<Lock>;}
namespace gfx {
struct Point {int x,y;Point(int a,int b):x(a),y(b){}bool operator==(const Point&)const=default;};
struct Size {int w,h;Size(int a,int b):w(a),h(b){}bool operator==(const Size&)const=default;};
struct Rect {int x=10,y=10;Size s{640,360};Rect()=default;Rect(int a,int b,int w,int h):x(a),y(b),s(w,h){}void set_size(Size v){s=v;}Size size()const{return s;}void set_origin(Point p){x=p.x;y=p.y;}Point origin()const{return {x,y};}};
}
namespace nativeguest {
enum {kGeometry=10};
struct ResizeEvent {uint32_t width,height;uint64_t buffer_size,reserved;};
struct MovedEvent {int32_t x,y;uint64_t reserved,reserved2;};
struct WindowCommand {unsigned operation;int x,y;unsigned width,height,stride;};
static bool failed=false;
static unsigned maximized;
static int64_t CommitWindowCommand(unsigned,WindowCommand *c){assert(c->operation==10);if(failed)return -1;c->x=1;c->y=41;c->width=1918;c->height=1038;c->stride=maximized;return 0;}
static bool Failed(int64_t n){return n<0;}
}
enum class PlatformWindowState {kNormal,kMaximized,kFullScreen,kMinimized};
struct Surface {base::Lock lock;gfx::Size size{640,360};uint64_t buffer_size=4096;std::atomic<bool> has_presented{true};};
struct NativeGuestWindow {
 bool valid=true,closed_=false,server_decorated_=true,destroy_on_state=false,top_level_=false;
 unsigned widget_=7,bounds_updates=0,state_updates=0;
 gfx::Rect bounds_;
 std::optional<gfx::Rect> restored_bounds_;
 PlatformWindowState window_state_=PlatformWindowState::kNormal;
 PlatformWindowState fullscreen_restore_state_=PlatformWindowState::kNormal;
 PlatformWindowState pre_minimize_state_=PlatformWindowState::kNormal;
 bool visible_=true;
 struct Delegate {unsigned activations=0;bool active=false;void OnActivationChanged(bool v){active=v;++activations;}} delegate,*delegate_=&delegate;
 Surface surface;Surface *surface_state_=&surface;
 struct Weak{std::shared_ptr<bool> alive;operator bool()const{return *alive;}};
 struct Factory{std::shared_ptr<bool> alive=std::make_shared<bool>(true);Weak GetWeakPtr(){return {alive};}}weak_ptr_factory_;
 bool IsValid(){return valid;}
 void UpdateBounds(gfx::Rect r){assert(*weak_ptr_factory_.alive);bounds_=r;++bounds_updates;}
 void UpdateWindowState(PlatformWindowState s){window_state_=s;++state_updates;if(destroy_on_state)*weak_ptr_factory_.alive=false;}
 void HandleResize(const nativeguest::ResizeEvent&);
 void HandleMoved(const nativeguest::MovedEvent&);
 void HandleMinimized(bool);
};
''' + body + r'''
int main(){
 nativeguest::ResizeEvent e{800,500,8192,0};NativeGuestWindow w;
 nativeguest::maximized=1;w.HandleResize(e);
 assert(w.window_state_==PlatformWindowState::kMaximized&&w.restored_bounds_&&w.bounds_.x==1&&w.bounds_.y==41&&w.bounds_.s.w==1918);
 assert(w.surface.size==w.bounds_.size()&&w.surface.buffer_size==8192&&!w.surface.has_presented);
 w.HandleResize(e);assert(w.state_updates==1&&w.bounds_updates==2);
 nativeguest::maximized=0;w.HandleResize(e);assert(w.window_state_==PlatformWindowState::kNormal&&!w.restored_bounds_);
 nativeguest::failed=true;w.HandleResize(e);assert(w.bounds_.s.w==800&&w.bounds_.s.h==500);nativeguest::failed=false;
 NativeGuestWindow client;client.server_decorated_=false;nativeguest::maximized=1;client.HandleResize(e);assert(!client.state_updates);
 NativeGuestWindow closing;closing.destroy_on_state=true;closing.HandleResize(e);assert(closing.state_updates==1&&!closing.bounds_updates);
 NativeGuestWindow invalid;invalid.valid=false;invalid.HandleResize(e);assert(!invalid.bounds_updates);
 w.HandleResize({0,500,8192,0});assert(w.bounds_updates==4);
 NativeGuestWindow closed;closed.closed_=true;closed.HandleResize(e);assert(!closed.bounds_updates&&!closed.state_updates);
 for(bool server:{false,true}){
  NativeGuestWindow hidden;hidden.server_decorated_=server;hidden.top_level_=true;
  hidden.window_state_=PlatformWindowState::kMinimized;
  for(unsigned i=0;i<1000;i++){
   nativeguest::maximized=2;hidden.HandleResize(e);
   assert(hidden.window_state_==PlatformWindowState::kMinimized&&hidden.pre_minimize_state_==PlatformWindowState::kFullScreen&&hidden.restored_bounds_);
   nativeguest::maximized=1;hidden.HandleResize(e);
   assert(hidden.window_state_==PlatformWindowState::kMinimized&&hidden.pre_minimize_state_==PlatformWindowState::kMaximized&&hidden.restored_bounds_);
   nativeguest::maximized=0;hidden.HandleResize(e);
   assert(hidden.window_state_==PlatformWindowState::kMinimized&&hidden.pre_minimize_state_==PlatformWindowState::kNormal&&!hidden.restored_bounds_);
  }
  assert(!hidden.state_updates&&hidden.bounds_updates==3000);
  hidden.HandleMinimized(false);
  assert(hidden.window_state_==PlatformWindowState::kNormal&&hidden.visible_&&hidden.delegate.active&&hidden.delegate.activations==1);
  hidden.HandleMinimized(true);assert(hidden.window_state_==PlatformWindowState::kMinimized&&!hidden.visible_);
  nativeguest::maximized=2;hidden.HandleResize(e);
  assert(hidden.window_state_==PlatformWindowState::kMinimized&&!hidden.visible_);
  hidden.HandleMinimized(false);
  assert(hidden.window_state_==PlatformWindowState::kFullScreen&&hidden.visible_&&hidden.delegate.active);
 }
 for(bool server:{false,true}){
  NativeGuestWindow full;full.server_decorated_=server;
  nativeguest::maximized=2;full.HandleResize(e);
  assert(full.window_state_==PlatformWindowState::kFullScreen&&full.restored_bounds_);
  full.HandleResize(e);assert(full.state_updates==1);
  nativeguest::maximized=0;full.HandleResize(e);
  assert(full.window_state_==PlatformWindowState::kNormal&&!full.restored_bounds_);
  full.window_state_=PlatformWindowState::kMaximized;full.restored_bounds_=gfx::Rect();
  nativeguest::maximized=3;full.HandleResize(e);
  assert(full.window_state_==PlatformWindowState::kFullScreen&&full.restored_bounds_);
  nativeguest::maximized=1;full.HandleResize(e);
  assert(full.window_state_==PlatformWindowState::kMaximized&&full.restored_bounds_);
 }
 for(bool server:{false,true}){
  NativeGuestWindow moved;moved.server_decorated_=server;
  for(int i=0;i<1000;i++)moved.HandleMoved({-100+i,40+i,0,0});
  assert(moved.bounds_.x==899&&moved.bounds_.y==1039&&moved.bounds_updates==1000);
  assert(moved.surface.size==gfx::Size(640,360)&&moved.surface.has_presented&&moved.surface.buffer_size==4096&&!moved.state_updates);
  moved.HandleMoved({899,1039,0,0});moved.HandleMoved({0,0,1,0});moved.HandleMoved({0,0,0,1});
  moved.closed_=true;moved.HandleMoved({0,0,0,0});moved.closed_=false;moved.valid=false;moved.HandleMoved({0,0,0,0});
  assert(moved.bounds_updates==1000);
 }
}
'''
with tempfile.TemporaryDirectory(prefix='chrome-resize-state-') as directory:
    base=Path(directory);(base/'test.cc').write_text(code)
    subprocess.run(['c++','-std=c++20','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie',str(base/'test.cc'),'-o',str(base/'test')],check=True)
    subprocess.run([str(base/'test')],check=True)
print('Chrome resize: native client origin, maximize/restore, latest geometry, query failure and callback-close safety passed')
