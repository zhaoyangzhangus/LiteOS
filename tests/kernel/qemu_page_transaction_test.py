"""Execute actual two-page repair/copy/flip with injected failure and cursor motion."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/graphics/present.c').read_text()
begin=source.index('#define WINDOW_QEMU_REPAIR_MAX_RECTS')
production=source[begin:]
code=r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <kernel/rect.h>
#define WINDOW_COMPOSITOR_PARALLEL_QEMU_COPY 0
#define WINDOW_CURSOR_WIDTH 2
#define WINDOW_CURSOR_HEIGHT 2
enum {STRIDE=40,HEIGHT=32};
static uint32_t scene[STRIDE*HEIGHT],pages[2][STRIDE*HEIGHT];
static struct {
 uint32_t display_width,display_height,display_stride,pointer_x,pointer_y;
 uint32_t presented_pointer_x,presented_pointer_y,presented_cursor_shape;
 bool presented_pointer_valid;
 volatile uint32_t *framebuffer,*composite_framebuffer;
} g_window_server;
static struct {uint32_t dragging_identifier,damage_count;Rect damage_rects[64];} g_compositor_snapshot;
static bool flip_failure,cursor_failure,locked;
static unsigned front_page;
static uint64_t copied;
static bool qemu_stdvga_flip_available(void){return true;}
static bool qemu_stdvga_page_flip_active(void){return true;}
static volatile uint32_t* qemu_stdvga_back_buffer(void){return pages[front_page^1];}
static volatile uint32_t* qemu_stdvga_front_buffer(void){return pages[front_page];}
static bool qemu_stdvga_flip(void){if(flip_failure)return false;front_page^=1;return true;}
static void window_lock(void){assert(!locked);locked=true;}
static void window_unlock(void){assert(locked);locked=false;}
static unsigned window_input_cursor_shape_locked(void){assert(locked);return 0;}
static int window_cursor_hotspot_x(unsigned shape){(void)shape;return 0;}
static int window_cursor_hotspot_y(unsigned shape){(void)shape;return 0;}
static bool window_present_cursor_into_buffer(volatile uint32_t* back,unsigned x,unsigned y,unsigned shape){
 (void)shape;
 for(unsigned row=y;row<y+2&&row<g_window_server.display_height;++row)
  for(unsigned col=x;col<x+2&&col<g_window_server.display_width;++col)
   back[row*STRIDE+col]=0xabcdef;
 return !cursor_failure; // Failure can occur after touching the hidden page.
}
static void window_display_set_scanout_locked(volatile uint32_t* front){g_window_server.framebuffer=front;}
static void compositor_copy_wc_scanline(volatile uint32_t* destination,const uint32_t* source,uint32_t width){
 copied+=width;for(unsigned i=0;i<width;++i)destination[i]=source[i];
}
''' + production + r'''
static bool frame_matches(void){
 for(unsigned y=0;y<g_window_server.display_height;++y)
  for(unsigned x=0;x<g_window_server.display_width;++x){
   bool cursor=x>=g_window_server.pointer_x&&x<g_window_server.pointer_x+2&&
               y>=g_window_server.pointer_y&&y<g_window_server.pointer_y+2;
   uint32_t expected=cursor?0xabcdef:scene[y*STRIDE+x];
   if(pages[front_page][y*STRIDE+x]!=expected)return false;
  }
 return true;
}
static void update(unsigned index,unsigned value){
 scene[index]=value;
 unsigned x=index%STRIDE,y=index/STRIDE;
 g_compositor_snapshot.damage_count=1;
 g_compositor_snapshot.damage_rects[0]=(Rect){x,y,x+1,y+1};
}
int main(void){
 for(unsigned i=0;i<STRIDE*HEIGHT;++i){scene[i]=100+i;pages[0][i]=pages[1][i]=0xdead;}
 g_window_server.display_width=32;g_window_server.display_height=HEIGHT;
 g_window_server.display_stride=STRIDE;
 g_window_server.framebuffer=pages[0];g_window_server.composite_framebuffer=scene;
 g_window_server.pointer_x=3;g_window_server.pointer_y=3;
 assert(compositor_commit_qemu_stdvga()&&frame_matches());
 assert(compositor_commit_qemu_stdvga()&&frame_matches());
 uint64_t baseline=copied;
 for(unsigned i=0;i<200;++i){
  g_window_server.pointer_x=i%28;g_window_server.pointer_y=(i*3)%28;
  update(((i*7)%32)+((i*11)%HEIGHT)*STRIDE,5000+i);
  assert(compositor_commit_qemu_stdvga()&&frame_matches());
 }
 assert(copied-baseline<200*32*HEIGHT/2);
 for(unsigned failure=0;failure<2;++failure){
  unsigned original=front_page;
  g_window_server.pointer_x=7;g_window_server.pointer_y=9;
  update(20*STRIDE+20,9000+failure);
  flip_failure=failure==0;cursor_failure=failure==1;
  assert(!compositor_commit_qemu_stdvga()&&front_page==original);
  flip_failure=cursor_failure=false;
  g_window_server.pointer_x=25;g_window_server.pointer_y=26;
  assert(compositor_commit_qemu_stdvga());
  if(!frame_matches())return 42; // Failed-attempt cursor must not survive retry.
  assert(compositor_commit_qemu_stdvga()&&frame_matches());
 }
 for(unsigned page=0;page<2;++page)for(unsigned y=0;y<HEIGHT;++y)
  for(unsigned x=32;x<STRIDE;++x)assert(pages[page][y*STRIDE+x]==0xdead);
}
'''
with tempfile.TemporaryDirectory(prefix='qemu-page-transaction-') as directory:
    base=Path(directory);(base/'test.c').write_text(code)
    command=['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(root/'include'),
                    str(base/'test.c'),'-o',str(base/'test')]
    subprocess.run(command,check=True)
    subprocess.run([str(base/'test')],check=True)
    # Removing only the fix reproduces the stale failed-attempt cursor, rather
    # than merely checking that some rectangle bookkeeping changed.
    begin=production.index('    /* The cursor changes the hidden page')
    end=production.index('    if (!window_present_cursor_into_buffer',begin)
    old=production[:begin]+production[end:]
    (base/'test.c').write_text(code.replace(production,old))
    subprocess.run(command,check=True)
    assert subprocess.run([str(base/'test')]).returncode==42
print('Actual QEMU page transaction: alternating pixels, sparse copy budget, failed flip/cursor retries and stride canaries passed')
