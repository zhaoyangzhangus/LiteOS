"""Execute actual snapshot finish: fence-specific wake, lock order, lifetime."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/graphics/compositor.c').read_text()
start=source.index('void compositor_snapshot_finish(void)')
opening=source.index('{',start)
depth=1;end=opening+1
while depth:
    depth+=(source[end]=='{')-(source[end]=='}');end+=1
finish=source[start:end]
program=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#define WINDOW_SERVER_MAX_WINDOWS 64
typedef struct window {
 int32_t compositor_presented_x,compositor_presented_y;
 uint32_t compositor_presented_width,compositor_presented_height;
 bool compositor_presented_valid;
 atomic_uint_fast64_t compositor_presented_sequence;
 unsigned event_waitq,puts;
 uint64_t expected;
 uint64_t present_sequence;
} window_server_window_t;
typedef struct {
 window_server_window_t *reference;
 void *section;
 int32_t x,y;
 uint32_t width,height,flags;
 uint64_t present_sequence;
} compositor_window_view_t;
static struct {
 compositor_window_view_t windows[WINDOW_SERVER_MAX_WINDOWS];
 uint32_t window_count,damage_count;
} g_compositor_snapshot;
static struct {bool composing;} g_window_server;
static bool locked,reset;
static unsigned wakes,puts;
static void window_lock(void){assert(!locked);locked=true;}
static void window_unlock(void){assert(locked);locked=false;}
static void window_surface_snapshot_release(compositor_window_view_t *view){assert(!locked&&view->reference);}
static uint32_t window_outer_width(uint32_t n,uint32_t f){(void)f;return n;}
static uint32_t window_outer_height(uint32_t n,uint32_t f){(void)f;return n;}
static void window_input_set_drag_blit_valid_locked(bool b){assert(locked&&!b);}
static int wake_all(unsigned *queue){
 assert(!locked&&!g_window_server.composing);
 window_server_window_t *found=0;
 for(unsigned i=0;i<g_compositor_snapshot.window_count;i++){
  window_server_window_t *w=g_compositor_snapshot.windows[i].reference;
  if(w && &w->event_waitq==queue){found=w;break;}
 }
 assert(found&&found->puts==0);
 assert(atomic_load_explicit(&found->compositor_presented_sequence,memory_order_acquire)==found->expected);
 ++*queue;++wakes;return 0;
}
static void object_put(window_server_window_t *w){assert(!locked);++w->puts;++puts;}
static void compositor_render_plan_reset(void){reset=true;}
''' + finish + r'''
int main(void){
 window_server_window_t windows[WINDOW_SERVER_MAX_WINDOWS]={0};
 g_compositor_snapshot.window_count=WINDOW_SERVER_MAX_WINDOWS;
 g_compositor_snapshot.damage_count=3;g_window_server.composing=true;
 for(unsigned i=0;i<WINDOW_SERVER_MAX_WINDOWS;i++){
  atomic_init(&windows[i].compositor_presented_sequence,10);
  windows[i].expected=i%4==0?11:10;
  g_compositor_snapshot.windows[i]=(compositor_window_view_t){
   .reference=&windows[i],.section=&windows[i],.x=12,.y=20,
   .width=640,.height=480,.present_sequence=windows[i].expected};
 }
 /* A sparse snapshot reference must never be woken or released. */
 g_compositor_snapshot.windows[4].reference=0;
 compositor_snapshot_finish();
 assert(wakes==15&&puts==63&&reset&&!locked);
 for(unsigned i=0;i<WINDOW_SERVER_MAX_WINDOWS;i++){
  assert(windows[i].event_waitq==(i!=4&&i%4==0));
  assert(g_compositor_snapshot.windows[i].reference==0);
  assert(g_compositor_snapshot.windows[i].section==0);
  if(i!=4)assert(windows[i].compositor_presented_valid);
 }
 assert(!g_compositor_snapshot.window_count&&!g_compositor_snapshot.damage_count);
 /* Cursor-only/unchanged frame has geometry bookkeeping but zero wakes. */
 wakes=puts=0;windows[0].puts=0;reset=false;
 g_compositor_snapshot.window_count=1;
 g_compositor_snapshot.windows[0].reference=&windows[0];
 g_window_server.composing=true;
 compositor_snapshot_finish();
 assert(wakes==0&&puts==1&&reset);
 /* Comparison keeps existing uint64 sequence publication semantics. */
 windows[0].puts=0;windows[0].expected=0;
 atomic_store(&windows[0].compositor_presented_sequence,UINT64_MAX);
 g_compositor_snapshot.windows[0].reference=&windows[0];
 g_compositor_snapshot.windows[0].present_sequence=0;
 g_compositor_snapshot.window_count=1;g_window_server.composing=true;
 compositor_snapshot_finish();assert(wakes==1);
 /* A newer submitted frame is not complete merely because an older
  * snapshot reached scanout. Publish only the sequence actually composed. */
 wakes=puts=0;windows[0].puts=0;windows[0].expected=41;
 windows[0].present_sequence=42;
 atomic_store(&windows[0].compositor_presented_sequence,40);
 g_compositor_snapshot.windows[0].reference=&windows[0];
 g_compositor_snapshot.windows[0].present_sequence=41;
 g_compositor_snapshot.window_count=1;g_window_server.composing=true;
 compositor_snapshot_finish();
 assert(wakes==1&&atomic_load(&windows[0].compositor_presented_sequence)==41);
 assert(atomic_load(&windows[0].compositor_presented_sequence)<windows[0].present_sequence);
 windows[0].puts=0;windows[0].expected=42;
 g_compositor_snapshot.windows[0].reference=&windows[0];
 g_compositor_snapshot.windows[0].present_sequence=42;
 g_compositor_snapshot.window_count=1;g_window_server.composing=true;
 compositor_snapshot_finish();
 assert(wakes==2&&atomic_load(&windows[0].compositor_presented_sequence)==42);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-compositor-wake-') as directory:
    path=Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('Actual compositor finish: unchanged fences skipped, changed fences published before wake, unlock before queue lock, references held, sparse/wrap cases passed')
