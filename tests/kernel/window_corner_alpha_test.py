"""Actual compositor outer-mask ownership, all corners, damage and clipping."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/graphics/render.c').read_text()
functions = source[source.index('static bool compositor_corner_pixel('):
                   source.index('void compositor_surface_locked(')]
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <uapi/window.h>
#include <uapi/window_theme.h>
#include <uapi/chrome_caption.h>
typedef struct {int32_t x,y;uint32_t width,height,flags;bool maximized;} compositor_window_view_t;
static uint32_t pixels[64*64],initial[64*64],foreground;
static struct {volatile uint32_t *composite_framebuffer;uint32_t display_width,display_height,display_stride;} g_window_server={pixels,64,64,64};
static struct {struct {uint32_t x0,y0,x1,y1;} damage_bounds;} g_compositor_snapshot;
static bool window_client_decorations(uint32_t flags){return flags&OS_WINDOW_CLIENT_DECORATIONS;}
static bool window_standard_caption(uint32_t flags){return flags&OS_WINDOW_STANDARD_CAPTION_CONTROLS;}
static uint32_t window_outer_width(uint32_t width,uint32_t flags){return width+(window_client_decorations(flags)?0:2);}
static uint32_t window_outer_height(uint32_t height,uint32_t flags){return height+(window_client_decorations(flags)?0:6);}
static bool compositor_damage_inside_surface_interior(const compositor_window_view_t *w){(void)w;return false;}
static void paint(const compositor_window_view_t *w){
 for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x)
  if((int)x>=w->x&&(int)y>=w->y&&(int64_t)x<(int64_t)w->x+window_outer_width(w->width,w->flags)&&
   (int64_t)y<(int64_t)w->y+window_outer_height(w->height,w->flags)&&
   x>=g_compositor_snapshot.damage_bounds.x0&&x<g_compositor_snapshot.damage_bounds.x1&&
   y>=g_compositor_snapshot.damage_bounds.y0&&y<g_compositor_snapshot.damage_bounds.y1)pixels[y*64+x]=foreground;
}
static void compositor_titlebar_locked(const compositor_window_view_t *w,uint32_t color){(void)color;paint(w);}
static void compositor_surface_locked(const compositor_window_view_t *w){paint(w);}
static void compositor_standard_caption_locked(const compositor_window_view_t *w){paint(w);}
''' + functions + r'''
static unsigned oracle_alpha(unsigned x,unsigned y,unsigned w,unsigned h){
 if(x>=w||y>=h)return 0;if(w<16||h<16)return 255;
 if(x>=w/2)x=w-1-x;if(y>=h/2)y=h-1-y;
 if(x>=8||y>=8)return 255;
 unsigned covered=0;
 for(int sy=0;sy<8;++sy)for(int sx=0;sx<8;++sx){int dx=128-(int)x*16-sx*2-1,dy=128-(int)y*16-sy*2-1;covered+=dx*dx+dy*dy<=128*128;}
 return (covered*255+32)/64;
}
static uint32_t blend(uint32_t a,uint32_t b,unsigned alpha){
 uint32_t out=b&0xFF000000;
 for(unsigned shift=0;shift<24;shift+=8)out|=((((a>>shift)&255)*alpha+((b>>shift)&255)*(255-alpha)+127)/255)<<shift;
 return out;
}
int main(void){
 for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x)assert(chrome_corner_coverage[y][x]==oracle_alpha(x,y,32,32));
 for(unsigned round=0;round<1000;++round){
  compositor_window_view_t w={.x=(int)(round%59)-12,.y=(int)(round%43)-10,
   .width=12+round%43,.height=12+round%39,.maximized=round%7==0,
   .flags=round%2?OS_WINDOW_CLIENT_DECORATIONS|OS_WINDOW_STANDARD_CAPTION_CONTROLS:0};
  unsigned width=window_outer_width(w.width,w.flags),height=window_outer_height(w.height,w.flags);
  g_compositor_snapshot.damage_bounds.x0=round%3?round%16:0;
  g_compositor_snapshot.damage_bounds.y0=round%4?round%13:0;
  g_compositor_snapshot.damage_bounds.x1=64-round%11;
  g_compositor_snapshot.damage_bounds.y1=64-round%9;
  foreground=(round*0x53917)&0xFFFFFF;
  for(unsigned i=0;i<4096;++i)pixels[i]=initial[i]=(i*0x72954)&0xFFFFFF;
  compositor_draw_window_locked(&w);
  for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x){
   uint32_t expected=initial[y*64+x];
   if((int)x>=w.x&&(int)y>=w.y&&(int64_t)x<(int64_t)w.x+width&&(int64_t)y<(int64_t)w.y+height&&
    x>=g_compositor_snapshot.damage_bounds.x0&&x<g_compositor_snapshot.damage_bounds.x1&&
    y>=g_compositor_snapshot.damage_bounds.y0&&y<g_compositor_snapshot.damage_bounds.y1){
    unsigned alpha=w.maximized?255:oracle_alpha(x-w.x,y-w.y,width,height);
    expected=blend(foreground,expected,alpha);
   }
   assert(pixels[y*64+x]==expected);
  }
 }
 puts("Compositor corners: exact 8x8 coverage, 1000 native/client/maximized windows, four symmetric corners, saved underlying pixels, frame drawn before final mask, offscreen/damage clipping and no app mask required passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-corner-alpha-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-misleading-indentation', '-fsanitize=address,undefined',
                    '-fno-pie', '-no-pie', '-I' + str(root / 'include'),
                    '-x', 'c', '-', '-o', binary], input=code, text=True, check=True)
    subprocess.run([binary], check=True)
