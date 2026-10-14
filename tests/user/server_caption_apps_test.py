"""Migrated apps own content only; execute actual caption and image metrics."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
for app in ('gshell', 'imageview', 'notepad', 'taskmgr', 'fileman', 'netmgr'):
    path = 'user/services/networkd/manager.c' if app == 'netmgr' else 'user/desktop/' + app + '/main.c'
    source = (root / path).read_text()
    for obsolete in ('OS_WINDOW_CLIENT_DECORATIONS', 'user_client_chrome_', 'USER_CLIENT_CHROME_'):
        assert obsolete not in source, (app, obsolete)
    for required in ('OS_WINDOW_STANDARD_CAPTION_CONTROLS', 'OS_WINDOW_EVENT_CLOSE_REQUEST',
                     'OS_WINDOW_CAPTION_HEIGHT(user_caption_height())'):
        assert required in source, (app, required)
image = (root / 'user/desktop/imageview/main.c').read_text()
fileman = (root / 'user/desktop/fileman/main.c').read_text()
assert 'FM4_CONTROL_CLOSE' not in fileman
assert 'fm4_window_frame' not in fileman
demo = (root / 'user/runtime/dyn_gfx.c').read_text()
assert 'OS_WINDOW_CLIENT_DECORATIONS' not in demo and 'gfx_frame(' not in demo
begin = image.index('static void reset_view(')
layout = image[begin:image.index('static void draw_image(', begin)]
code = r'''
#include <assert.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>
#include "user/runtime/caption.h"
#include "user/runtime/paint.h"
static uint32_t font_height;
uint32_t ui_text_line_height(void){return font_height;}
#define IMAGEVIEW_STATUS_HEIGHT 32U
static struct {uint32_t width,height;} g_window;
static uint32_t g_zoom_percent;
static int32_t g_pan_x,g_pan_y;
static uint32_t received_width,received_height;
static uint32_t fit_zoom(uint32_t w,uint32_t h){received_width=w;received_height=h;return 75;}
static void clamp_pan(uint32_t w,uint32_t h){received_width=w;received_height=h;}
''' + layout + r'''
int main(void) {
    uint32_t surface[34*32+2];
    for(uint32_t w=1;w<=32;++w) for(uint32_t h=1;h<=32;++h) {
        for(unsigned i=0;i<sizeof(surface)/sizeof(*surface);++i)surface[i]=42;
        user_paint_round_rect(surface+1,34,w,h,0,0,UINT_MAX,UINT_MAX,7);
        assert(surface[0]==42&&surface[34*h+1]==42);
        for(uint32_t y=0;y<h;++y)for(uint32_t x=w;x<34;++x)
            assert(surface[1+y*34+x]==42);
        user_paint_round_rect(surface+1,34,w,h,UINT_MAX,UINT_MAX,32,32,7);
    }
    struct {char before;char title[32];char after;} caption={.before=42,.after=43};
    char path[256];memset(path,'p',255);path[255]=0;
    user_caption_format_title(caption.title,path,true);
    assert(caption.before==42&&caption.after==43&&caption.title[0]=='*'&&caption.title[1]==' '&&caption.title[31]==0);
    user_caption_format_title(caption.title,"/notes.txt",false);
    assert(!strcmp(caption.title,"/notes.txt"));
    for(unsigned i=11;i<32;++i)assert(!caption.title[i]);
    user_caption_format_title(caption.title,NULL,false);assert(!caption.title[0]);
    for(font_height=0;font_height<256;++font_height) {
        uint32_t expected=font_height>=112?128:font_height>24?font_height+16:40;
        assert(user_caption_height()==expected);
    }
    font_height=UINT_MAX;assert(user_caption_height()==128);
    for(uint32_t height=0;height<1200;++height) {
        g_window.width=640;g_window.height=height;g_pan_x=10;g_pan_y=20;
        reset_view();
        assert(g_zoom_percent==75&&!g_pan_x&&!g_pan_y&&received_width==640);
        assert(received_height==(height>32?height-32:1));
        change_zoom(25);assert(g_zoom_percent==100&&received_height==(height>32?height-32:1));
        change_zoom(-100);assert(g_zoom_percent==10);
        change_zoom(1000);assert(g_zoom_percent==400);
    }
}
'''
with tempfile.TemporaryDirectory(prefix='server-caption-apps-') as directory:
    base = Path(directory)
    (base / 'test.c').write_text(code)
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I' + str(root), '-I' + str(root / 'include'),
                    str(base / 'test.c'), '-o', str(base / 'test')], check=True)
    subprocess.run([str(base / 'test')], check=True, timeout=15)
print('Server captions: migrated apps have no self-drawn chrome, bounded font metrics and 1200 image client-area/zoom layouts passed')
