"""Run the production damage accumulator against a per-pixel coverage oracle."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
app = (root / 'user/desktop/fileman/main.c').read_text()
code = app[app.index('static void fm4_damage_all('):app.index('static void fill_rect(')]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define FM4_DAMAGE_CAPACITY 24U
typedef struct {uint32_t x,y,width,height;} fm4_damage_rect_t;
static fm4_damage_rect_t g_damage[FM4_DAMAGE_CAPACITY];
static uint32_t g_damage_count,g_target_width=64,g_target_height=48;
static bool g_damage_full;
static struct {uint32_t width,height;}g_window={64,48};
''' + code + r'''
static uint32_t seed=5;
static uint32_t next(void){seed=seed*1664525U+1013904223U;return seed;}
int main(void){
 fm4_damage_reset();
 for(unsigned i=0;i<24;++i)fm4_damage_rect(i%8*4,i/8*4,1,1);
 assert(!g_damage_full && g_damage_count==24);
 fm4_damage_rect(0,0,1,1);
 assert(!g_damage_full && g_damage_count==24);
 fm4_damage_rect(0,0,5,1);
 assert(!g_damage_full && g_damage_count==23);
 fm4_damage_rect(63,47,1,1);
 assert(!g_damage_full && g_damage_count==24);
 fm4_damage_rect(60,40,1,1);
 assert(g_damage_full && !g_damage_count);
 /* Growing the candidate must revisit earlier non-touching regions. */
 fm4_damage_reset();
 fm4_damage_rect(0,0,1,1);fm4_damage_rect(3,0,1,1);
 fm4_damage_rect(6,0,1,1);fm4_damage_rect(1,0,5,1);
 assert(!g_damage_full && g_damage_count==1);
 assert(g_damage[0].x==0 && g_damage[0].width==7);
 for(unsigned trial=0;trial<1000;++trial){
  unsigned char expected[48][64]={0};fm4_damage_reset();
  for(unsigned step=0;step<40;++step){
   uint32_t x=next()%80,y=next()%60,w=next()%12,h=next()%12;
   if(step%11==0)w=UINT32_MAX;
   fm4_damage_rect(x,y,w,h);
   for(unsigned py=0;py<48;++py)for(unsigned px=0;px<64;++px)
    if(px>=x && py>=y && (uint64_t)px<x+(uint64_t)w &&
       (uint64_t)py<y+(uint64_t)h)expected[py][px]=1;
   assert(g_damage_count<=FM4_DAMAGE_CAPACITY);
   for(unsigned i=0;i<g_damage_count;++i){
    fm4_damage_rect_t a=g_damage[i];
    assert(a.width && a.height && a.x+a.width<=64 && a.y+a.height<=48);
    for(unsigned j=0;j<i;++j)assert(!fm4_damage_touches(&a,&g_damage[j]));
   }
   for(unsigned py=0;py<48;++py)for(unsigned px=0;px<64;++px)
    if(expected[py][px])assert(fm4_damage_contains(px,py));
  }
 }
 assert(!fm4_damage_intersects(0,0,0,1));
}
'''
with tempfile.TemporaryDirectory(prefix='fileman-regions-') as directory:
    source = Path(directory) / 'test.c'
    source.write_text(program)
    binary = Path(directory) / 'test'
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('Fileman damage: full-capacity duplicate/bridge, overflow fallback, transitive merge, 40000 updates PASS')
