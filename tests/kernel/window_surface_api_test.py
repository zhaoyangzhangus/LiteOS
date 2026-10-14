"""Run production surface BEGIN/COMMIT/CANCEL with injected wait/close/resize races."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/graphics/window.c').read_text()
start = source.index('typedef struct window_surface_wait_context {')
end = source.index('kstatus_t window_server_geometry_owned(', start)
production = source[start:end]
program = r'''
#include <assert.h>
#include <stdatomic.h>
#include <string.h>
#include "kernel/window_surface_exchange.h"
#include "uapi/window_surface.h"
#define K_OK 0
#define K_EINVAL -22
#define K_EPERM -1
#define K_ECANCELED -125
#define K_EAGAIN -11
#define K_ETIMEDOUT -110
#define OS_WINDOW_VISIBLE 1
typedef int kstatus_t;
typedef struct {int id;} process_t;
typedef struct {
 process_t *owner;
 window_surface_exchange_t surface_exchange;
 bool surface_exchange_enabled,resize_pending,dirty;
 uint64_t owner_address,surface_span,buffer_size,present_sequence;
 uint32_t width,height,surface_writer_width,surface_writer_height,flags;
 int32_t x,y;
 atomic_bool registry_removed;
 unsigned surface_waitq;
} window_server_window_t;
static window_server_window_t w;
static bool locked;
static unsigned mode,wakes,marks,notifies,shows;
static void window_lock(void){assert(!locked);locked=true;}
static void window_unlock(void){assert(locked);locked=false;}
static bool window_server_owner_locked(process_t *p,window_server_window_t *v){
 assert(locked);return p==v->owner&&!atomic_load(&v->registry_removed);
}
static unsigned wake_all(unsigned *q){assert(!locked&&q==&w.surface_waitq);return ++wakes;}
static void window_mark_surface_locked(window_server_window_t *v,int x,int y,unsigned width,unsigned height){
 assert(locked&&v==&w&&x==0&&y==0&&width==w.width&&height==w.height);
 assert(w.present_sequence&& !w.resize_pending && w.dirty);++marks;
}
static void window_server_notify_worker(void){assert(!locked);++notifies;}
static kstatus_t window_server_set(window_server_window_t *v,int x,int y,unsigned visible){
 assert(!locked&&v==&w&&x==v->x&&y==v->y&&visible==1);++shows;v->flags|=visible;return 0;
}
static kstatus_t wait_on_queue(unsigned *q,bool (*predicate)(void *),void *context,uint64_t timeout){
 assert(!locked&&q==&w.surface_waitq&&timeout==5000000000ULL);
 if(mode==1){assert(!predicate(context));return K_ETIMEDOUT;}
 if(mode==2){assert(!predicate(context));assert(window_surface_read_end(&w.surface_exchange,0));}
 assert(predicate(context));
 uint64_t serial=w.surface_exchange.serial;
 assert(predicate(context)&&w.surface_exchange.serial==serial); // Idempotent predicate.
 if(mode==3)return K_ECANCELED; // Cancellation after acquisition must abort it.
 if(mode==4){atomic_store(&w.registry_removed,true);window_surface_exchange_close(&w.surface_exchange);}
 return K_OK;
}
''' + production + r'''
int main(void){
 process_t owner={1},other={2};
 w=(window_server_window_t){.owner=&owner,.surface_exchange_enabled=true,
  .surface_span=4096,.buffer_size=8192,.owner_address=0x20000,.width=16,.height=16};
 os_surface_frame_t frame;
 assert(window_server_surface_begin(&other,&w,&frame)==K_EPERM&&!frame.token);
 assert(window_server_surface_begin(&owner,&w,&frame)==K_OK);
 assert(frame.address==0x21000&&frame.capacity==4096&&frame.stride==64);
 os_surface_frame_t bad=frame;bad.address++;
 assert(window_server_surface_end(&owner,&w,&bad,true)==K_EINVAL&&w.surface_exchange.writing);
 bad=frame;bad.token++;
 assert(window_server_surface_end(&owner,&w,&bad,false)==K_EINVAL&&w.surface_exchange.writing);
 assert(window_server_surface_end(&owner,&w,&frame,true)==K_OK);
 assert(w.surface_exchange.front==1&&marks==1&&notifies==1&&shows==1);
 assert(window_server_surface_end(&owner,&w,&frame,true)==K_EINVAL);
 w.surface_exchange.readers[0]=1;mode=1;
 assert(window_server_surface_begin(&owner,&w,&frame)==K_ETIMEDOUT&&!frame.token);
 mode=2;assert(window_server_surface_begin(&owner,&w,&frame)==K_OK);
 assert(frame.address==0x20000);w.width=8;w.resize_pending=true;
 assert(window_server_surface_end(&owner,&w,&frame,true)==K_EAGAIN);
 assert(w.surface_exchange.front==1&&!w.surface_exchange.writing&&w.resize_pending&&marks==1);
 mode=0;assert(window_server_surface_begin(&owner,&w,&frame)==K_OK);
 assert(frame.width==8&&frame.stride==32);
 assert(window_server_surface_end(&owner,&w,&frame,false)==K_OK&&w.surface_exchange.front==1);
 mode=3;assert(window_server_surface_begin(&owner,&w,&frame)==K_ECANCELED);
 assert(!frame.token&&!w.surface_exchange.writing);
 mode=0;w.surface_exchange.serial=UINT64_MAX;
 assert(window_server_surface_begin(&owner,&w,&frame)==K_EINVAL);
 w.surface_exchange.serial=10;w.width=2048;
 assert(window_server_surface_begin(&owner,&w,&frame)==K_EINVAL);
 w.width=8;mode=4;
 assert(window_server_surface_begin(&owner,&w,&frame)==K_ECANCELED);
 assert(!frame.token&&!w.surface_exchange.writing);
 mode=0;assert(window_server_surface_begin(&owner,&w,&frame)==K_ECANCELED);
 assert(!locked&&wakes);
}
'''
with tempfile.TemporaryDirectory(prefix='window-surface-api-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I', str(root / 'include'), str(path / 'test.c'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True, timeout=20)
print('Surface API: ownership, slot/stride bounds, stale token, atomic full commit, '
      'timeout, reader retry, resize abort, cancellation after acquire and close passed')
