"""Compile production capture ownership/routing with boundary and lifecycle tests."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
window=(root/'kernel/graphics/window.c').read_text()
router=(root/'kernel/graphics/input_router.c').read_text()
owned=window[window.index('kstatus_t window_server_capture_owned('):window.index('kstatus_t window_server_set_minimized_owned(')]
route=router[router.index('static bool window_route_relative_pointer_locked('):router.index('void route_input_locked(')]
program=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
enum {K_OK=0,K_EINVAL=-22,K_EPERM=-1,K_EBUSY=-16,
 INPUT_EVENT_RELATIVE=1,INPUT_EVENT_BUTTON=2,INPUT_EVENT_KEY=3,
 INPUT_REL_X=0,INPUT_REL_Y=1,INPUT_REL_WHEEL=8,OS_WINDOW_VISIBLE=1,
 OS_WINDOW_EXPLICIT_MOVE_RESIZE=64,INPUT_BUTTON_LEFT=1,INPUT_VALUE_RELEASE=0,INPUT_VALUE_PRESS=1};
typedef int kstatus_t;
typedef struct {int id;} process_t;
typedef struct {int type,code,value;} input_event_t;
typedef struct {process_t* owner; uint32_t flags; bool client_pointer_capture,client_button_capture,client_hidden,minimized;uint64_t present_sequence;} window_server_window_t;
static struct {uint32_t count,pointer_x,pointer_y,display_width,display_height; window_server_window_t* windows[4];} g_window_server;
static int locks, delivered,damage,coalesces,pointer_updates; static bool ready=true;
static window_server_window_t* last;
static void window_lock(void){assert(locks==0);++locks;}
static void window_unlock(void){assert(locks==1);--locks;}
static bool window_server_kernel_ready(void){return ready;}
static bool window_server_owner_locked(process_t* p,window_server_window_t* w){assert(locks==1);return w->owner==p;}
static void window_input_set_pointer_locked(uint32_t x,uint32_t y){++pointer_updates;g_window_server.pointer_x=x;g_window_server.pointer_y=y;}
static void window_mark_moved_cursor_locked(uint32_t a,uint32_t b,uint32_t c,uint32_t d){(void)a;(void)b;(void)c;(void)d;++damage;}
static void window_coalesce_damage_locked(void){++coalesces;}
static void window_enqueue_event_locked(window_server_window_t* w,const input_event_t* e){(void)e;last=w;++delivered;}
'''+owned+route+r'''
int main(void){
 process_t p={1},foreign={2};
 window_server_window_t a={.owner=&p,.flags=OS_WINDOW_VISIBLE},b={.owner=&p,.flags=OS_WINDOW_VISIBLE};
 g_window_server.count=2;g_window_server.windows[0]=&a;g_window_server.windows[1]=&b;
 g_window_server.display_width=1920;g_window_server.display_height=1080;
 assert(window_server_capture_owned(0,&a,1)==K_EINVAL);
 assert(window_server_capture_owned(&foreign,&a,1)==K_EPERM && !a.client_pointer_capture);
 assert(window_server_capture_owned(&p,&a,2)==K_EINVAL);
 assert(window_server_capture_owned(&p,&a,1)==K_OK);
 assert(window_server_capture_owned(&p,&a,1)==K_OK);
 assert(window_server_capture_owned(&p,&b,1)==K_EBUSY);
 assert(window_server_capture_owned(&p,&b,0)==K_OK && a.client_pointer_capture);
 input_event_t e={INPUT_EVENT_RELATIVE,INPUT_REL_X,2500};
 assert(route_client_capture_locked(&e) && last==&a && g_window_server.pointer_x==1919);
 e.code=INPUT_REL_Y;e.value=-500;assert(route_client_capture_locked(&e) && g_window_server.pointer_y==0);
 e.code=INPUT_REL_Y;e.value=1500;assert(route_client_capture_locked(&e) && g_window_server.pointer_y==1079);
 e.type=INPUT_EVENT_BUTTON;assert(route_client_capture_locked(&e) && delivered==4 && damage==2);
 assert(pointer_updates==2 && coalesces==2);
 e.type=INPUT_EVENT_RELATIVE;e.code=INPUT_REL_X;e.value=INT32_MAX;
 for(unsigned i=0;i<10000;++i)assert(route_client_capture_locked(&e));
 assert(pointer_updates==2 && damage==2 && coalesces==2 && delivered==10004);
 e.code=INPUT_REL_WHEEL;assert(route_client_capture_locked(&e));
 assert(pointer_updates==2 && damage==2 && coalesces==2 && delivered==10005);
 e.code=7;assert(route_client_capture_locked(&e)); // unknown axis still delivered
 assert(pointer_updates==2 && damage==2 && coalesces==2 && delivered==10006);
 e.type=INPUT_EVENT_KEY;assert(!route_client_capture_locked(&e));
 e.type=INPUT_EVENT_BUTTON;
 a.client_hidden=true;assert(!route_client_capture_locked(&e));
 assert(window_server_capture_owned(&p,&a,1)==K_EINVAL);
 a.client_hidden=false;a.minimized=true;assert(!route_client_capture_locked(&e));
 a.minimized=false;a.flags=0;a.present_sequence=1;assert(!route_client_capture_locked(&e));
 assert(window_server_capture_owned(&p,&a,1)==K_EINVAL);
 assert(window_server_capture_owned(&p,&a,0)==K_OK);
 // Show defers exposing the first surface, not capture ownership/routing.
 a.present_sequence=0;
 assert(window_server_capture_owned(&p,&a,1)==K_OK && a.client_pointer_capture);
 assert(route_client_capture_locked(&e) && last==&a);
 assert(window_server_capture_owned(&p,&b,1)==K_EBUSY);
 assert(window_server_capture_owned(&p,&a,0)==K_OK);
 a.flags=OS_WINDOW_VISIBLE;
 assert(window_server_capture_owned(&p,&a,0)==K_OK && !route_client_capture_locked(&e));
 a.client_button_capture=true;e.code=INPUT_BUTTON_LEFT;e.value=1;
 assert(route_client_capture_locked(&e) && last==&a && a.client_button_capture);
 e.value=INPUT_VALUE_RELEASE;
 assert(route_client_capture_locked(&e) && !a.client_button_capture);
 assert(!route_client_capture_locked(&e));
 // A same-process popup can take over the parent's implicit press grab.
 // No explicit or foreign capture may be stolen, and a later array conflict
 // must not clear an earlier same-owner grab before returning failure.
 a.client_button_capture=true;
 assert(window_server_capture_owned(&p,&b,0)==K_OK&&a.client_button_capture);
 b.client_hidden=true;
 assert(window_server_capture_owned(&p,&b,1)==K_EINVAL&&a.client_button_capture);
 b.client_hidden=false;a.client_pointer_capture=true;
 assert(window_server_capture_owned(&p,&b,1)==K_EBUSY&&a.client_button_capture);
 a.client_pointer_capture=false;a.owner=&foreign;
 assert(window_server_capture_owned(&p,&b,1)==K_EBUSY&&a.client_button_capture);
 a.owner=&p;
 window_server_window_t c={.owner=&foreign,.flags=OS_WINDOW_VISIBLE,.client_pointer_capture=true};
 g_window_server.windows[2]=&c;g_window_server.count=3;
 assert(window_server_capture_owned(&p,&b,1)==K_EBUSY&&a.client_button_capture&&!b.client_pointer_capture);
 c.client_pointer_capture=false;c.client_button_capture=true;
 assert(window_server_capture_owned(&p,&b,1)==K_EBUSY&&a.client_button_capture&&c.client_button_capture);
 c.owner=&p;
 assert(window_server_capture_owned(&p,&b,1)==K_OK&&b.client_pointer_capture);
 assert(!a.client_button_capture&&!c.client_button_capture);
 assert(route_client_capture_locked(&e)&&last==&b); // The popup receives release.
 assert(window_server_capture_owned(&p,&b,0)==K_OK);
 g_window_server.count=2;g_window_server.windows[2]=0;
 g_window_server.windows[0]=0;
 assert(window_server_capture_owned(&p,&b,1)==K_OK && route_client_capture_locked(&e) && last==&b);
 g_window_server.count=1;assert(!route_client_capture_locked(&e)); // removed window
 assert(locks==0);
}
'''
assert 'if (route_client_capture_locked(event)) return;' in router
assert 'window->client_pointer_capture = false;' in router
assert 'if (visible == 0U && !defer_first_surface) {\n        window->client_pointer_capture = false;' in window
with tempfile.TemporaryDirectory() as directory:
    cpp=Path(directory)/'capture.c';exe=Path(directory)/'capture'
    cpp.write_text(program)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(cpp),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('Pointer capture: ownership, exclusive/idempotent acquire, release, desktop routing, clamping, keyboard exclusion and lifecycle passed')
