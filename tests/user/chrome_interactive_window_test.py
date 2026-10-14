"""Execute the nativeguest WM handler using real Chromium hit-test and ABI headers."""
from pathlib import Path
import subprocess
import sys
import tempfile
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from chromium_source import chromium_source

root=chromium_source()
source=(root/'ui/ozone/platform/nativeguest/nativeguest_window.cc').read_text()
begin=source.index('void NativeGuestWindow::DispatchHostWindowDragMovement(')
handler=source[begin:source.index('bool NativeGuestWindow::ShouldUseNativeFrame()',begin)]
assert 'SetWmMoveResizeHandler(this, this);' in source
assert 'SetWmMoveResizeHandler(this, nullptr);' in source
code=r'''
#include <cassert>
#include <cstdint>
#include <initializer_list>
#include "ui/base/hit_test.h"
#include "ui/ozone/platform/nativeguest/nativeguest_window_abi.h"
namespace gfx {struct Point {};}
namespace ui {
enum {EF_LEFT_MOUSE_BUTTON=16,EF_RIGHT_MOUSE_BUTTON=32};
struct NativeGuestWindowManager {
 int buttons=0;
 int& mouse_button_flags(){return buttons;}
};
struct NativeGuestWindow;
struct Delegate {
 NativeGuestWindow* window=nullptr;
 unsigned lost=0;
 bool destroy=false;
 void OnLostCapture();
};
struct NativeGuestWindow {
 bool valid=true,closed_=false,top_level_=true,has_capture_=true,capture_pending_=false,accept=true;
 unsigned commits=0,last_edges=0;
 int mouse_button_flags_=0;
 NativeGuestWindowManager* manager_=nullptr;
 Delegate* delegate_;
 bool IsValid(){return valid;}
 bool HasCapture(){return has_capture_||capture_pending_;}
 bool Commit(const nativeguest::WindowCommand& command){
  assert(command.operation==13&&!command.reserved&&!command.reserved2);
  assert(!command.x&&!command.y&&!command.height&&!command.stride&&!command.address&&!command.length);
  ++commits;last_edges=command.width;return accept;
 }
 void DispatchHostWindowDragMovement(int,const gfx::Point&);
};
void Delegate::OnLostCapture(){
 assert(window&&!window->has_capture_&&!window->capture_pending_);
 ++lost;
 if(destroy){delete window;window=nullptr;}
}
''' + handler + r'''
}
int main(){
 static_assert(ui::nativeguest::kInteractive==13);
 static_assert(ui::nativeguest::kWindowExplicitMoveResize==64);
 ui::Delegate delegate;
 ui::NativeGuestWindow window;
 delegate.window=&window;window.delegate_=&delegate;
 const int hits[]={HTCAPTION,HTLEFT,HTRIGHT,HTTOP,HTTOPLEFT,HTTOPRIGHT,HTBOTTOM,HTBOTTOMLEFT,HTBOTTOMRIGHT};
 const unsigned edges[]={0,1,2,4,5,6,8,9,10};
 ui::NativeGuestWindowManager manager;
 for(bool shared:{false,true}){
 window.manager_=shared?&manager:nullptr;
 for(unsigned i=0;i<9;++i){
  for(unsigned capture=0;capture<4;++capture){
   window.has_capture_=capture&1;window.capture_pending_=capture&2;
   int& buttons=shared?manager.buttons:window.mouse_button_flags_;
   buttons=ui::EF_LEFT_MOUSE_BUTTON|ui::EF_RIGHT_MOUSE_BUTTON;
   unsigned before=window.commits,lost=delegate.lost;
   window.accept=false;window.DispatchHostWindowDragMovement(hits[i],{});
   assert(window.commits==before+1&&delegate.lost==lost);
   assert(window.has_capture_==bool(capture&1)&&window.capture_pending_==bool(capture&2));
   assert(buttons==(ui::EF_LEFT_MOUSE_BUTTON|ui::EF_RIGHT_MOUSE_BUTTON));
   window.accept=true;window.DispatchHostWindowDragMovement(hits[i],{});
   assert(window.commits==before+2&&window.last_edges==edges[i]);
   assert(!window.HasCapture()&&delegate.lost==lost+bool(capture));
   assert(buttons==ui::EF_RIGHT_MOUSE_BUTTON);
  }
 }
 }
 unsigned before=window.commits;
 const int invalid_hits[]={HTCLIENT,HTCLOSE,HTMAXBUTTON,HTMINBUTTON,HTSYSMENU,HTNOWHERE,HTBORDER,-99,999};
 for(int hit:invalid_hits)
  window.DispatchHostWindowDragMovement(hit,{});
 assert(window.commits==before);
 window.valid=false;window.DispatchHostWindowDragMovement(HTCAPTION,{});window.valid=true;
 window.closed_=true;window.DispatchHostWindowDragMovement(HTCAPTION,{});window.closed_=false;
 window.top_level_=false;window.DispatchHostWindowDragMovement(HTCAPTION,{});
 assert(window.commits==before);
 delegate.destroy=true;
 auto* transient=new ui::NativeGuestWindow;
 transient->delegate_=&delegate;delegate.window=transient;
 transient->DispatchHostWindowDragMovement(HTCAPTION,{});
 assert(!delegate.window);
}
'''
with tempfile.TemporaryDirectory(prefix='chrome-interactive-') as directory:
    base=Path(directory)
    (base/'test.cc').write_text(code)
    subprocess.run(['c++','-std=c++20','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root),str(base/'test.cc'),'-o',str(base/'test')],check=True)
    subprocess.run([str(base/'test')],check=True)
print('Chrome interactive WM: nine hit targets, zeroed ABI, failed/successful capture handoff, controls and popup exclusion passed')
