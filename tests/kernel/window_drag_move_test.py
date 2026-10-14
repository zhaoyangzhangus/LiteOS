"""Execute the shared production drag mutation used by both input paths."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/graphics/input_drag.c').read_text()
function = 'void window_drag_move_locked(' + source.split(
    'void window_drag_move_locked(', 1)[1].split('uint32_t window_resize_edges_locked(', 1)[0]
assert source.count('window_drag_move_locked(dragged, new_x, new_y);') == 1
router = (root / 'kernel/graphics/input_router.c').read_text()
assert router.count('window_drag_move_locked(dragged, new_x, new_y);') == 1
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
typedef struct {
 int32_t x,y,compositor_presented_x,compositor_presented_y;
 uint32_t width,height,flags;bool dirty;
} window_server_window_t;
static unsigned safety_checks,records,damages,repairs;
static unsigned notifications;
static bool safe;
static int32_t expected_x,expected_y,expected_new_x,expected_new_y;
static uint32_t expected_width,expected_height;
static void window_enqueue_moved_event_locked(window_server_window_t *w){
 assert(w&&w->x==expected_new_x&&w->y==expected_new_y);++notifications;
}
static unsigned window_outer_width(unsigned w,unsigned f){(void)f;return w+2;}
static unsigned window_outer_height(unsigned h,unsigned f){(void)f;return h+32;}
static bool window_drag_reuse_safe_locked(const window_server_window_t *w,
 int32_t ox,int32_t oy,int32_t nx,int32_t ny,unsigned width,unsigned height){
 assert(w&&ox==w->compositor_presented_x&&oy==w->compositor_presented_y);
 assert(nx==expected_new_x&&ny==expected_new_y);
 assert(width==expected_width&&height==expected_height);++safety_checks;return safe;
}
static void window_input_record_drag_frame_locked(bool reuse,int32_t ox,int32_t oy,
 int32_t nx,int32_t ny,unsigned width,unsigned height){
 assert(reuse==safe&&ox==expected_x&&oy==expected_y);
 assert(nx==expected_new_x&&ny==expected_new_y);
 assert(width==expected_width&&height==expected_height);++records;
}
static void window_mark_moved_rect_locked(int32_t ox,int32_t oy,int32_t nx,int32_t ny,
 unsigned width,unsigned height,bool full){
 assert(full==!safe&&ox==expected_x&&oy==expected_y);
 assert(nx==expected_new_x&&ny==expected_new_y);
 assert(width==expected_width&&height==expected_height);++damages;
}
static void window_mark_drag_corner_repair_locked(int32_t x,int32_t y,unsigned w,unsigned h,unsigned f){
 assert(safe&&x==expected_new_x&&y==expected_new_y&&w==expected_width&&h==expected_height&&f==7);++repairs;
}
''' + function + r'''
int main(void){
 window_server_window_t w={.x=100,.y=200,.width=800,.height=600,.flags=7,
  .compositor_presented_x=80,.compositor_presented_y=180};
 window_drag_move_locked(0,10,20);
 window_drag_move_locked(&w,100,200);
 assert(!safety_checks&&!records&&!damages&&!repairs&&!notifications&&!w.dirty);
 expected_width=802;expected_height=632;
 for(unsigned i=0;i<2;i++){
  safe=i!=0;w.dirty=false;
  expected_x=safe?w.compositor_presented_x:w.x;
  expected_y=safe?w.compositor_presented_y:w.y;
  expected_new_x=i==0?-20:120;expected_new_y=i==0?-50:250;
  window_drag_move_locked(&w,expected_new_x,expected_new_y);
  assert(w.x==expected_new_x&&w.y==expected_new_y&&w.dirty);
  assert(safety_checks==i+1&&records==i+1&&damages==i+1&&repairs==i);
  assert(notifications==i+1);
  window_drag_move_locked(&w,w.x,w.y);
  assert(safety_checks==i+1&&records==i+1&&damages==i+1&&repairs==i);
  assert(notifications==i+1);
 }
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-drag-move-') as directory:
    base = Path(directory)
    source_path, binary = base / 'test.c', base / 'test'
    source_path.write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(source_path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('Shared drag mutation: both callers, null/no-op, negative coordinates, full damage, committed-source reuse passed')
