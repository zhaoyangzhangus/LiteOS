"""Exercise production file-manager text damage and runtime rectangle clipping."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
app = (root / 'user/desktop/fileman/main.c').read_text()
runtime = (root / 'user/runtime/text.c').read_text()
draw = app[app.index('static void draw_text('):app.index('static bool set_path(')]
clip = runtime[runtime.index('void ui_text_draw_clipped('):
               runtime.index('uint32_t ui_text_measure_range(')]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef struct {int x,y,w,h;} BLRectI;
typedef struct {int unused;} BLContextCookie;
enum {BL_SUCCESS=0};
static struct {int context;} g_text;
static BLRectI current, saved;
static uint32_t *target, stride;
static int saves, restores, draws;
static bool open_context(uint32_t *p,uint32_t s,uint32_t w,uint32_t h) {
 if(!p || !w || !h) return false;
 target=p;stride=s;current=(BLRectI){0,0,(int)w,(int)h};return true;
}
static int bl_context_save(int *c,BLContextCookie *k) {
 (void)c;(void)k;saved=current;++saves;return 0;
}
static int bl_context_clip_to_rect_i(int *c,const BLRectI *r) {
 (void)c;current=*r;return 0;
}
static int bl_context_restore(int *c,const BLContextCookie *k) {
 (void)c;(void)k;current=saved;++restores;return 0;
}
static size_t text_length(const char *t) {return strlen(t);}
/* A partially covered glyph pixel darkens if drawn repeatedly. */
static void draw_text_range(int32_t x,int32_t y,const char *t,size_t n,uint32_t c) {
 (void)t;(void)c;++draws;
 for(size_t i=0;i<n;++i) for(int r=0;r<2;++r) {
  int px=x+(int)i,py=y+r;
  if(px>=current.x && py>=current.y && px<current.x+current.w && py<current.y+current.h)
   target[py*stride+px]/=2;
 }
}
static void ui_text_draw(uint32_t *p,uint32_t s,uint32_t w,uint32_t h,
 int32_t x,int32_t y,const char *t,uint32_t c) {
 if(open_context(p,s,w,h))draw_text_range(x,y,t,text_length(t),c);
}
void ui_text_draw_rect(uint32_t*,uint32_t,uint32_t,uint32_t,int32_t,int32_t,
 uint32_t,uint32_t,uint32_t,uint32_t,const char*,uint32_t);
typedef struct {uint32_t x,y,width,height;} fm4_damage_rect_t;
static uint32_t pixels[16*8],baseline[16*8];
static uint32_t *g_target=pixels,g_target_width=16,g_target_height=8;
static fm4_damage_rect_t g_damage[4];
static uint32_t g_damage_count;
static bool g_damage_full;
''' + clip + draw + r'''
int main(void) {
 for(unsigned i=0;i<128;++i)pixels[i]=255;
 g_damage_full=true;draw_text(2,2,"abcdef",0);
 memcpy(baseline,pixels,sizeof pixels);
 g_damage_full=false;g_damage_count=1;
 g_damage[0]=(fm4_damage_rect_t){10,0,6,8};
 for(int i=0;i<100;++i)draw_text(2,2,"abcdef",0);
 assert(memcmp(baseline,pixels,sizeof pixels)==0);
 /* A partial glyph refresh must affect only cleared damaged pixels. */
 g_damage[0]=(fm4_damage_rect_t){4,3,2,1};
 pixels[3*16+4]=pixels[3*16+5]=255;
 draw_text(2,2,"abcdef",0);
 assert(memcmp(baseline,pixels,sizeof pixels)==0);
 /* Disjoint damage rectangles must not repeatedly blend one glyph. */
 g_damage_count=2;g_damage[0]=(fm4_damage_rect_t){2,2,2,2};
 g_damage[1]=(fm4_damage_rect_t){6,2,2,2};
 for(int y=2;y<4;++y)for(int x=2;x<8;++x)if(x<4||x>=6)pixels[y*16+x]=255;
 draw_text(2,2,"abcdef",0);
 assert(memcmp(baseline,pixels,sizeof pixels)==0);
 int before=draws;
 ui_text_draw_rect(pixels,16,16,8,2,2,16,0,99,99,"abc",0);
 ui_text_draw_rect(pixels,16,16,8,2,2,0,8,99,99,"abc",0);
 assert(before==draws);
 assert(saves==restores && current.x==0 && current.w==16);
}
'''
with tempfile.TemporaryDirectory(prefix='fileman-text-damage-') as directory:
    path = Path(directory)
    source = path / 'test.c'
    source.write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(source), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('Fileman text: repeated hover, partial glyph damage, disjoint clips and clip restoration PASS')
