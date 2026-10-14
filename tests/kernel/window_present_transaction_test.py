"""Run production present/update with a compositor snapshot at every unlock."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/graphics/window.c').read_text()

def function(signature):
    start = source.rindex(signature)
    end = source.index('{',start)+1
    depth = 1
    while depth:
        depth += (source[end]=='{')-(source[end]=='}')
        end += 1
    return source[start:end]

program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#define OS_ENABLE_FRAME_TRACE 0
#define K_OK 0
#define K_EINVAL -22
#define K_EPERM -1
#define K_EIO -5
#define K_ECANCELED -125
#define OS_WINDOW_CAPTION_HEIGHT_MASK (0xFFU<<16U)
#define OS_WINDOW_VISIBLE 1U
typedef int kstatus_t;
typedef struct {int id;} process_t;
typedef struct {
 process_t *owner;
 uint32_t identifier,width,height,flags;
 int32_t x,y;
 uint64_t owner_address,buffer_size,present_sequence;
 atomic_uint_fast64_t compositor_presented_sequence;
 atomic_bool registry_removed;
 bool dirty,resize_pending;
 bool surface_exchange_enabled;
 int event_waitq;
} window_server_window_t;
typedef struct {
 window_server_window_t *window;
 uint64_t sequence;
} window_present_wait_context_t;
static window_server_window_t window;
static bool window_standard_caption(uint32_t flags){return (flags&20U)==20U;}
static bool ready=true,inject_snapshot=true;
static int locked,marks,premature,notifies,waits,shows;
static int last_x,last_y;
static uint32_t last_width,last_height;
static uint64_t last_observed_sequence;
static int last_observed_marks;
static void snapshot(void) {
 assert(!locked);
 if(window.present_sequence!=last_observed_sequence) {
  if(marks==last_observed_marks)++premature;
  last_observed_sequence=window.present_sequence;
  last_observed_marks=marks;
  atomic_store(&window.compositor_presented_sequence,window.present_sequence);
 }
}
static void window_lock(void){assert(!locked);locked=1;}
static void window_unlock(void){assert(locked);locked=0;if(inject_snapshot)snapshot();}
static bool window_server_kernel_ready(void){return ready;}
static window_server_window_t *window_scene_find_locked(uint32_t id){assert(locked);return id==window.identifier?&window:0;}
static bool window_server_owner_locked(process_t *p,window_server_window_t *w){assert(locked);return w==&window && p==w->owner && w->identifier;}
static void window_mark_window_locked(window_server_window_t *w){assert(locked && w==&window);++marks;}
static void window_mark_surface_locked(window_server_window_t *w,int x,int y,uint32_t width,uint32_t height){
 assert(locked && w==&window);++marks;last_x=x;last_y=y;last_width=width;last_height=height;
}
static void window_server_notify_worker(void){assert(!locked);++notifies;}
static kstatus_t window_server_set(window_server_window_t *w,int x,int y,uint32_t visible){
 assert(!locked && w==&window && x==w->x && y==w->y && visible==1);++shows;w->flags|=OS_WINDOW_VISIBLE;return 0;
}
static kstatus_t wait_on_queue(int *queue,bool (*predicate)(void*),void *raw,uint64_t timeout){
 assert(!locked && queue==&window.event_waitq && timeout==5000000000ULL);++waits;
 assert(predicate(raw));return 0;
}
''' + '\n'.join((
    function('static bool window_present_sequence_ready('),
    function('static kstatus_t window_present_wait_result('),
    function('static kstatus_t window_server_update_internal('),
    function('kstatus_t window_server_update('),
    function('kstatus_t window_server_present('),
)) + r'''
int main(void) {
 process_t owner={1},other={2};
 window=(window_server_window_t){.owner=&owner,.identifier=7,.width=100,.height=100,
  .flags=OS_WINDOW_VISIBLE,.x=10,.y=10,.owner_address=4096,.buffer_size=40000};
 // Reproduce the old two-lock publication. A snapshot can acknowledge the
 // new sequence at the first unlock, before any submitted damage exists.
 window_lock();window.present_sequence=1;window_unlock();
 assert(premature==1 && atomic_load(&window.compositor_presented_sequence)==1);
 window_lock();window_mark_surface_locked(&window,1,2,3,4);window_unlock();
 window.present_sequence=0;atomic_store(&window.compositor_presented_sequence,0);
 marks=premature=last_observed_marks=0;last_observed_sequence=0;
 for(uint64_t sequence=1;sequence<=128;++sequence) {
  assert(window_server_present(&owner,&window,4096,40000,400,1,2,3,4)==0);
  assert(premature==0 && window.present_sequence==sequence);
  assert(atomic_load(&window.compositor_presented_sequence)==sequence);
  assert(last_x==1 && last_y==2 && last_width==3 && last_height==4);
 }
 assert(waits==128 && notifies==128 && marks==128);
 const uint64_t sequence=window.present_sequence;
 assert(window_server_update(&owner,7,0,0,0,0,0)==0);
 assert(window.present_sequence==sequence && marks==129);
 window.resize_pending=true;window.flags=0;
 assert(window_server_present(&owner,&window,4096,40000,400,0,0,100,100)==0);
 assert(!window.resize_pending && shows==1 && window.flags==OS_WINDOW_VISIBLE);
 const int saved_marks=marks,saved_waits=waits;
 const uint64_t saved_sequence=window.present_sequence;
 assert(window_server_present(&other,&window,4096,40000,400,0,0,100,100)==K_EPERM);
 assert(window_server_present(&owner,&window,4097,40000,400,0,0,100,100)==K_EINVAL);
 assert(window_server_present(&owner,&window,4096,39999,400,0,0,100,100)==K_EINVAL);
 assert(window_server_present(&owner,&window,4096,40000,399,0,0,100,100)==K_EINVAL);
 assert(window_server_present(&owner,&window,4096,40000,400,99,0,2,1)==K_EINVAL);
 ready=false;
 assert(window_server_present(&owner,&window,4096,40000,400,0,0,100,100)==K_EIO);
 ready=true;
 assert(window.present_sequence==saved_sequence && marks==saved_marks && waits==saved_waits);
 assert(window_server_update(&other,7,0,0,3,4,0)==K_EPERM);
 assert(window_server_update(&owner,7,0,0,3,0,0)==K_EINVAL);
 assert(window_server_update(&owner,7,0,0,3,4,1)==K_EINVAL);
 window.flags=21U;
 assert(window_server_update(&owner,7,0,0,3,4,76U<<16U)==0);
 assert((window.flags&OS_WINDOW_CAPTION_HEIGHT_MASK)==(76U<<16U));
 assert(window_server_update(&owner,7,0,0,3,4,39U<<16U)==K_EINVAL);
 assert(window_server_update(&owner,7,0,0,3,4,129U<<16U)==K_EINVAL);
 assert(window_server_update(&owner,7,0,0,3,4,101U<<16U)==K_EINVAL);
 assert(premature==0 && !locked);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-present-transaction-') as directory:
    path=Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie',
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('present transaction: old premature ACK reproduced; production damage/fence atomic, ownership/validation/resize/visibility passed (ASan/UBSan)')
