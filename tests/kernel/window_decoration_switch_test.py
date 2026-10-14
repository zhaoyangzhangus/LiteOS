"""Run production frame switching: ownership, fixed outer geometry and input reset."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root/'kernel/graphics/window.c').read_text()
begin = source.index('kstatus_t window_server_set_decorations_owned(')
body = source[begin:source.index('kstatus_t window_server_present(',begin)]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <uapi/window.h>
typedef int process_t,kstatus_t;
enum {K_OK=0,K_EINVAL=-22,K_EPERM=-1,WINDOW_MIN_WIDTH=160,WINDOW_MIN_HEIGHT=96};
typedef struct {process_t *owner;unsigned identifier,flags,width,height,restore_width,restore_height;uint64_t buffer_size,surface_span;bool surface_exchange_enabled,maximized,dirty,resize_pending,fullscreen;} window_server_window_t;
static struct {unsigned title_pressed_identifier,dragging_identifier;} g_window_server;
static bool ready=true,registered=true,locked;
static unsigned marks,events,wakes,captures,drags;
static bool window_server_kernel_ready(void){return ready;}
static void window_lock(void){assert(!locked);locked=true;}
static void window_unlock(void){assert(locked);locked=false;}
static bool window_server_owner_locked(process_t *p,window_server_window_t *w){assert(locked);return registered&&w->owner==p;}
static unsigned window_frame_extra(unsigned f){return f&OS_WINDOW_CLIENT_DECORATIONS?0:2;}
static unsigned window_titlebar_height(unsigned f){return f&OS_WINDOW_CLIENT_DECORATIONS?0:os_window_caption_height(f);}
static unsigned window_outer_width(unsigned w,unsigned f){return w+window_frame_extra(f);}
static unsigned window_outer_height(unsigned h,unsigned f){return h+window_frame_extra(f)+window_titlebar_height(f);}
static void window_mark_window_locked(window_server_window_t *w){assert(locked&&w);++marks;}
static void window_enqueue_resize_event_locked(window_server_window_t *w){assert(locked&&w&&w->resize_pending);++events;}
static void window_clear_title_capture_locked(void){assert(locked);g_window_server.title_pressed_identifier=0;++captures;}
static void window_input_clear_drag_locked(void){assert(locked);g_window_server.dragging_identifier=0;++drags;}
static void window_server_notify_worker(void){assert(!locked);++wakes;}
''' + body + r'''
int main(void){
 process_t owner,other;
 for(unsigned caption=40;caption<=128;++caption)for(unsigned maximized=0;maximized<2;++maximized){
  unsigned kept=OS_WINDOW_VISIBLE|OS_WINDOW_RESIZABLE|OS_WINDOW_PREMULTIPLIED_ALPHA|OS_WINDOW_CAPTION_HEIGHT(caption);
  window_server_window_t w={.owner=&owner,.identifier=7,.flags=kept|OS_WINDOW_STANDARD_CAPTION_CONTROLS,
   .width=640,.height=360,.restore_width=320,.restore_height=240,.buffer_size=32*1024*1024,.maximized=maximized};
  for(unsigned i=0;i<100;++i){
   unsigned ow=window_outer_width(w.width,w.flags),oh=window_outer_height(w.height,w.flags);
   g_window_server.title_pressed_identifier=7;g_window_server.dragging_identifier=7;
   unsigned n=wakes;
   assert(!window_server_set_decorations_owned(&owner,&w,0)&&wakes==n+1);
   assert(w.width==ow&&w.height==oh&&w.flags==(kept|OS_WINDOW_CLIENT_DECORATIONS));
   assert(!g_window_server.dragging_identifier&&!g_window_server.title_pressed_identifier);
   assert(!window_server_set_decorations_owned(&owner,&w,0)&&wakes==n+1);
   assert(!window_server_set_decorations_owned(&owner,&w,1)&&wakes==n+2);
   assert(w.width==640&&w.height==360&&w.restore_width==320&&w.restore_height==240);
   assert(window_outer_width(w.width,w.flags)==ow&&window_outer_height(w.height,w.flags)==oh);
   assert((w.flags&~OS_WINDOW_STANDARD_CAPTION_CONTROLS)==kept);
  }
  unsigned n=wakes;
  assert(window_server_set_decorations_owned(&other,&w,0)==-1&&wakes==n);
  registered=false;assert(window_server_set_decorations_owned(&owner,&w,0)==-1);registered=true;
  ready=false;assert(window_server_set_decorations_owned(&owner,&w,0)==-22);ready=true;
  assert(window_server_set_decorations_owned(&owner,&w,2)==-22);
  w.buffer_size=640*360*4;window_server_window_t insufficient=w;
  assert(window_server_set_decorations_owned(&owner,&w,0)==-22&&!memcmp(&insufficient,&w,sizeof(w)));
  w.surface_exchange_enabled=true;w.surface_span=w.buffer_size;w.buffer_size*=2;
  insufficient=w;
  assert(window_server_set_decorations_owned(&owner,&w,0)==-22&&!memcmp(&insufficient,&w,sizeof(w)));
  w.surface_span=w.buffer_size;
  g_window_server.title_pressed_identifier=7;g_window_server.dragging_identifier=7;
  assert(!window_server_set_decorations_owned(&owner,&w,0));
  assert(!window_server_set_decorations_owned(&owner,&w,1));
  w.surface_exchange_enabled=false;
  w.buffer_size=32*1024*1024;
  w.flags=kept|OS_WINDOW_CLIENT_DECORATIONS;w.width=100;w.height=50;
  window_server_window_t before=w;
  assert(window_server_set_decorations_owned(&owner,&w,1)==-22&&!memcmp(&before,&w,sizeof(w)));
  w.width=640;w.height=360;w.restore_width=1;w.restore_height=1;w.maximized=true;before=w;
  assert(window_server_set_decorations_owned(&owner,&w,1)==-22&&!memcmp(&before,&w,sizeof(w)));
 }
 assert(events==wakes&&marks==2*wakes&&captures*2==wakes&&drags==captures);
}
'''
with tempfile.TemporaryDirectory(prefix='window-decoration-switch-') as directory:
    base = Path(directory)
    (base/'test.c').write_text(code)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root/'include'),str(base/'test.c'),'-o',str(base/'test')],check=True)
    subprocess.run([str(base/'test')],check=True)
print('Frame switching: 17800 round trips, stable outer/restore geometry, ownership, input cancellation and no-op wake suppression passed')
