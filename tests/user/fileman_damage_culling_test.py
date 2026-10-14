"""Exercise production card culling and its real file-grid call site."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
app = (root / 'user/desktop/fileman/main.c').read_text()
helper = app[app.index('static bool fm4_damage_intersects('):
             app.index('static bool fm4_damage_touches(')]
grid = app[app.index('    /*\n     * File grid.'):app.index('    /*\n     * Empty folder.')]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
typedef struct { uint32_t x,y,width,height; } fm4_damage_rect_t;
static bool g_damage_full;
static uint32_t g_damage_count;
static fm4_damage_rect_t g_damage[4];
''' + helper + r'''
enum { FM4_GRID_CARD_WIDTH=126, FM4_GRID_CARD_HEIGHT=126,
       OS_FILE_TYPE_DIRECTORY=1, OS_THEME_SELECTED=1,
       OS_THEME_HOVER=2, OS_THEME_TEXT=3 };
static uint32_t g_first_entry,g_entry_count=16,g_selected,g_hover_item=99;
static bool g_selection_valid;
static struct {struct {unsigned type;char name[2];}info;}g_entries[16];
static unsigned icons,labels,highlights;
static unsigned fm4_columns(void){return 4;}
static unsigned fm4_visible_rows(void){return 4;}
static bool fm4_item_rect(unsigned i,unsigned *x,unsigned *y){
 *x=(i%4)*148;*y=(i/4)*140;return i<16;
}
static void fm4_round_rect(unsigned x,unsigned y,unsigned w,unsigned h,unsigned c){
 (void)x;(void)y;(void)w;(void)h;(void)c;++highlights;
}
static void fm4_folder_icon(unsigned x,unsigned y){(void)x;(void)y;++icons;}
static void fm4_file_icon(unsigned x,unsigned y){(void)x;(void)y;++icons;}
static void fm4_item_label(unsigned x,unsigned y,const char *n,unsigned c){
 (void)x;(void)y;(void)n;(void)c;++labels;
}
static void render_grid(void){
''' + grid + r'''
}
static uint32_t rng=42;
static uint32_t next(void){rng=rng*1664525U+1013904223U;return rng;}
int main(void){
 g_damage_count=1;g_damage[0]=(fm4_damage_rect_t){10,10,10,10};
 assert(!fm4_damage_intersects(0,0,10,10)); /* touching edge is not damage */
 assert(fm4_damage_intersects(19,19,1,1));
 assert(!fm4_damage_intersects(20,20,1,1));
 assert(!fm4_damage_intersects(10,10,0,1));
 assert(!fm4_damage_intersects(10,10,1,0));
 g_damage[0].width=0;assert(!fm4_damage_intersects(0,0,40,40));
 g_damage_count=0;assert(!fm4_damage_intersects(0,0,40,40));
 g_damage_full=true;assert(fm4_damage_intersects(UINT32_MAX,UINT32_MAX,1,1));
 for(unsigned n=0;n<20000;++n){
  uint32_t x=next(),y=next(),w=next(),h=next();
  g_damage_count=4;g_damage_full=(n%7==0);
  bool expected=g_damage_full && w && h;
  for(unsigned i=0;i<4;++i){
   g_damage[i]=(fm4_damage_rect_t){next(),next(),next(),next()};
   fm4_damage_rect_t r=g_damage[i];
   uint64_t l=x>r.x?x:r.x,t=y>r.y?y:r.y;
   uint64_t a=(uint64_t)x+w,b=(uint64_t)r.x+r.width;
   uint64_t c=(uint64_t)y+h,d=(uint64_t)r.y+r.height;
   expected|=w && h && r.width && r.height && l<(a<b?a:b) && t<(c<d?c:d);
  }
  assert(fm4_damage_intersects(x,y,w,h)==expected);
 }
 g_damage_full=true;assert(!fm4_damage_intersects(0,0,0,1));
 render_grid();assert(icons==16 && labels==16);
 g_damage_full=false;g_damage_count=1;
 g_damage[0]=(fm4_damage_rect_t){148,140,126,126};
 icons=labels=0;g_selected=5;g_selection_valid=true;
 render_grid();assert(icons==1 && labels==1 && highlights==1);
 g_damage[0]=(fm4_damage_rect_t){127,0,20,126};
 icons=labels=0;render_grid();assert(icons==0 && labels==0);
 g_damage[0]=(fm4_damage_rect_t){125,139,24,2};
 icons=labels=0;render_grid();assert(icons==2 && labels==2);
 g_damage_count=2;g_damage[0]=(fm4_damage_rect_t){0,0,1,1};
 g_damage[1]=(fm4_damage_rect_t){444,420,1,1};
 icons=labels=0;render_grid();assert(icons==2 && labels==2);
}
'''
with tempfile.TemporaryDirectory(prefix='fileman-culling-') as directory:
    source = Path(directory) / 'test.c'
    source.write_text(program)
    binary = Path(directory) / 'test'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    # A removed call-site gate must not silently turn this into a helper-only
    # test: the real grid must fail its per-card work-count assertions.
    gate = '''if (!fm4_damage_intersects(x, y, FM4_GRID_CARD_WIDTH,
                                   FM4_GRID_CARD_HEIGHT)) continue;'''
    assert program.count(gate) == 1
    source.write_text(program.replace(gate, '', 1))
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    str(source), '-o', str(binary)], check=True)
    import resource
    def disable_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    negative = subprocess.run([str(binary)], capture_output=True,
                              preexec_fn=disable_core)
    assert negative.returncode != 0, 'Removed grid culling escaped the regression test'
print('Fileman culling: 20000 wide-coordinate cases; actual grid full/partial/gap/multi-damage PASS')
