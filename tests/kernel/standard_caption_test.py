"""Exercise production caption hit testing and client maximize/restore geometry."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/graphics/input_router.c').read_text()
def function(signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <kernel/window_geometry.h>
#include <uapi/chrome_caption.h>
#define DESKTOP_TOPBAR_HEIGHT 0U
#define WINDOW_RESIZE_GRAB 8U
#define WINDOW_RESIZE_LEFT 1U
#define WINDOW_RESIZE_RIGHT 2U
#define WINDOW_RESIZE_TOP 4U
#define WINDOW_RESIZE_BOTTOM 8U
typedef struct {
 uint32_t flags,identifier,width,height,restore_width,restore_height;
 int32_t x,y,restore_x,restore_y;
 bool maximized,dirty,resize_pending,fullscreen;
} window_server_window_t;
static struct { uint32_t display_width,display_height,dragging_identifier; } g_window_server={1920,1080,0};
static unsigned marks,resizes;
static void window_mark_window_locked(window_server_window_t *w) { assert(w); ++marks; }
static void window_input_clear_drag_locked(void) { g_window_server.dragging_identifier=0; }
static void window_enqueue_resize_event_locked(window_server_window_t *w) { assert(w->resize_pending); ++resizes; }
''' + (root/'kernel/graphics/input.c').read_text().replace('#include "internal.h"','') + function('uint32_t window_title_button_at_locked(') + '\n' + function('static bool window_toggle_maximize_locked(') + r'''
int main(void) {
 const uint32_t flags=OS_WINDOW_VISIBLE|OS_WINDOW_RESIZABLE|OS_WINDOW_CLIENT_DECORATIONS|OS_WINDOW_STANDARD_CAPTION_CONTROLS;
 window_server_window_t w={.flags=flags,.identifier=1,.x=240,.y=135,.width=1440,.height=810};
 for (unsigned button=1;button<=3;++button) {
  int x,y; assert(window_caption_button_rect(flags,w.x,w.y,w.width,button,&x,&y));
  assert(x==w.x+(int)w.width-(4-(int)button)*32 && y==w.y);
  assert(window_title_button_at_locked(&w,x,y)==0); // top resize strip
  assert(window_title_button_at_locked(&w,x+16,y+20)==button);
  assert(window_title_button_at_locked(&w,x+31,y+39)==(button==3?0:button));
  assert(window_title_button_at_locked(&w,x+16,y+40)==0);
 }
 assert(window_title_button_at_locked(&w,1583,155)==0);
 w.flags=flags|OS_WINDOW_CAPTION_HEIGHT(76U);
 assert(window_title_button_at_locked(&w,1664,210)==3);
 assert(window_title_button_at_locked(&w,1664,211)==0);
 w.flags=flags;
 assert(window_title_button_at_locked(0,1664,155)==0);
 w.flags &= ~OS_WINDOW_STANDARD_CAPTION_CONTROLS;
 assert(window_title_button_at_locked(&w,1664,155)==0);
 assert(window_toggle_maximize_locked(&w)); // Programmatic CSD maximize shares geometry policy.
 assert(window_toggle_maximize_locked(&w));
 marks=resizes=0;
 w.flags=flags;
 g_window_server.dragging_identifier=1;
 assert(window_toggle_maximize_locked(&w));
 assert(w.maximized && w.x==0 && w.y==0 && w.width==1920 && w.height==1080);
 assert(w.resize_pending && g_window_server.dragging_identifier==0);
 assert(window_toggle_maximize_locked(&w));
 assert(!w.maximized && w.x==240 && w.y==135 && w.width==1440 && w.height==810);
 assert(resizes==2 && marks==4);
 w.fullscreen=true;assert(!window_toggle_maximize_locked(&w));w.fullscreen=false;
 w.flags &= ~OS_WINDOW_RESIZABLE;
 assert(!window_toggle_maximize_locked(&w));
 unsigned ink[5]={0};
 for (unsigned icon=1;icon<=4;++icon)
  for (unsigned y=0;y<12;++y) for(unsigned x=0;x<12;++x) ink[icon]+=chrome_caption_alpha(icon,x,y);
 assert(ink[1]==20*255 && ink[2]==64*255 && ink[3]>0 && ink[4]>0);
 assert(chrome_caption_alpha(1,12,0)==0 && chrome_caption_alpha(3,0,12)==0);
 assert(chrome_frame_inset(0,320)==4 && chrome_frame_inset(8,320)==0);
 assert(chrome_frame_row_inset(199,320,200)==4 && chrome_frame_alpha(0,199,320,200)==0);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-caption-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I' + str(root / 'include'), str(path / 'test.c'),
                    str(root / 'kernel/graphics/window_geometry.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('Standard caption: production hit boundaries, Chrome opt-out, maximize/restore and vector paths passed')
