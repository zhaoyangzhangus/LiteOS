"""Production resize policy versus an independent client-size oracle."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'kernel/graphics/input_drag.c').read_text()
start = source.index('static void window_resize_axis(')
end = source.index('bool window_route_drag_motion_batch_locked(', start)
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <limits.h>
#define OS_WINDOW_RESIZABLE 1U
#define CLIENT 2U
#define WINDOW_RESIZE_LEFT 1U
#define WINDOW_RESIZE_RIGHT 2U
#define WINDOW_RESIZE_TOP 4U
#define WINDOW_RESIZE_BOTTOM 8U
#define WINDOW_MIN_WIDTH 160U
#define WINDOW_MIN_HEIGHT 96U
typedef struct {int32_t x,y;uint32_t width,height,flags;bool dirty,resize_pending;} window_server_window_t;
static struct {uint32_t display_width,display_height,resize_edges;} g_window_server;
static unsigned frame_queries,title_queries,marks,events,title_height;
static uint32_t frame(uint32_t f){return f&CLIENT?0:2;}
static uint32_t title(uint32_t f){return f&CLIENT?0:title_height;}
static uint32_t window_frame_extra(uint32_t f){++frame_queries;return frame(f);}
static uint32_t window_titlebar_height(uint32_t f){++title_queries;return title(f);}
static uint32_t window_outer_width(uint32_t w,uint32_t f){return w+frame(f);}
static uint32_t window_outer_height(uint32_t h,uint32_t f){return h+frame(f)+title(f);}
static void window_mark_rect_locked(int32_t x,int32_t y,uint32_t w,uint32_t h){(void)x;(void)y;(void)w;(void)h;++marks;}
static void window_mark_window_locked(window_server_window_t *w){assert(w);++marks;}
static void window_enqueue_resize_event_locked(window_server_window_t *w){assert(w);++events;}
''' + source[start:end] + r'''
static void reference_axis(int32_t *position,uint32_t *size,int32_t delta,
                           unsigned edges,unsigned near,unsigned far,
                           uint32_t limit,uint32_t minimum,uint32_t decoration){
 if(!(edges&(near|far)))return;
 bool moving_near=(edges&near)!=0;
 int64_t wanted=(int64_t)*size+(moving_near?-(int64_t)delta:delta);
 uint32_t lower=limit<minimum?limit:minimum;
 if(wanted<lower)wanted=lower;
 if(wanted>limit)wanted=limit;
 if(moving_near){
  int64_t fixed_end=(int64_t)*position+*size+decoration;
  *position=(int32_t)(fixed_end-wanted-decoration);
 }
 *size=(uint32_t)wanted;
}
static uint32_t seed=43;
static uint32_t next(void){seed=seed*1664525U+1013904223U;return seed>>8;}
int main(void){
 unsigned cases=0;
 for(unsigned trial=0;trial<10000;++trial){
  g_window_server.display_width=1+next()%4000;
  g_window_server.display_height=1+next()%2400;
  title_height=30+next()%100;
  int32_t dx=(int32_t)(next()%8000)-4000,dy=(int32_t)(next()%4800)-2400;
  if(trial%100==0){dx=INT32_MIN;dy=INT32_MAX;}
  window_server_window_t original={.x=(int32_t)(next()%5000)-1000,
    .y=(int32_t)(next()%3000)-800,.width=1+next()%4000,.height=1+next()%2400,
    .flags=OS_WINDOW_RESIZABLE|((trial&1)?CLIENT:0)};
  for(unsigned edges=0;edges<16;++edges){
   window_server_window_t actual=original,expected=original;
   g_window_server.resize_edges=edges;
   reference_axis(&expected.x,&expected.width,dx,edges,1,2,
                  g_window_server.display_width,160,frame(original.flags));
   reference_axis(&expected.y,&expected.height,dy,edges,4,8,
                  g_window_server.display_height,96,frame(original.flags)+title(original.flags));
   bool changed=actual.x!=expected.x||actual.y!=expected.y||
                actual.width!=expected.width||actual.height!=expected.height;
   marks=events=frame_queries=title_queries=0;
   assert(window_resize_locked(&actual,dx,dy)==changed);
   assert(actual.x==expected.x&&actual.y==expected.y&&actual.width==expected.width&&actual.height==expected.height);
   assert(actual.dirty==changed&&actual.resize_pending==changed);
   assert(marks==(changed?2U:0U)&&events==(changed?1U:0U));
   assert(frame_queries==(edges?1U:0U)&&title_queries==(edges?1U:0U));
   ++cases;
  }
 }
 window_server_window_t w={.flags=OS_WINDOW_RESIZABLE,.width=800,.height=600};
 g_window_server.resize_edges=2;g_window_server.display_width=0;
 assert(!window_resize_locked(&w,1,1));
 assert(!window_resize_locked(0,1,1));
 g_window_server.display_width=1920;w.flags=0;
 assert(!window_resize_locked(&w,1,1));
 printf("Resize geometry: %u equivalent cases, all edges, clamps, decorations, extreme deltas and no-op publication passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-resize-geometry-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-x', 'c', '-', '-o', binary], input=code, text=True, check=True)
    subprocess.run([binary], check=True)
