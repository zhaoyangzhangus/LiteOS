"""Production window producers versus a linear FIFO oracle at every ring boundary."""
from pathlib import Path
import argparse
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--negative-controls', action='store_true')
args = parser.parse_args()
source = (root/'kernel/graphics/input_events.c').read_text()
pointer = source[source.index('static int32_t window_pointer_clamp_i32('):
                 source.index('void window_event_reset_ready_locked(')]
producers = source[source.index('/* All producers share'):]
dispatch_source = (root/'kernel/graphics/window.c').read_text()
dispatch = dispatch_source[dispatch_source.index('kstatus_t window_server_dispatch('):
                           dispatch_source.index('typedef struct window_event_wait_context')]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "uapi/window.h"
#define WINDOW_EVENT_CAPACITY 64U
#define K_OK 0
#define K_EINVAL -22
#define K_ENOENT -2
typedef int kstatus_t;
typedef os_input_event_t input_event_t;
typedef struct {
 uint32_t identifier,flags,width,height;int32_t x,y;uint64_t buffer_size;
 bool minimized;void *owner;
 uint32_t event_read,event_write,event_count;
 uint64_t canary_before;os_window_event_t events[64];uint64_t canary_after;
} window_server_window_t;
static struct {uint32_t pointer_x,pointer_y;int event_waitq;} g_window_server;
static unsigned wakes,locked,global_wakes,flushes;
static bool initialized=true;
static window_server_window_t *registered;
static void window_lock(void){assert(!locked);locked=1;}
static void window_unlock(void){assert(locked);locked=0;}
static bool window_server_init(void){return initialized;}
static window_server_window_t *window_scene_find_locked(uint32_t id){assert(locked);return registered && registered->identifier==id?registered:NULL;}
static bool window_flush_event_wakes(void){assert(!locked);++flushes;return true;}
static unsigned wake_all(void *q){assert(!locked && q);++global_wakes;return 1;}
static uint32_t window_client_offset_x(uint32_t f){return f&OS_WINDOW_CLIENT_DECORATIONS?0:1;}
static uint32_t window_client_offset_y(uint32_t f){return f&OS_WINDOW_CLIENT_DECORATIONS?0:41;}
static void window_event_schedule_wake_locked(window_server_window_t *w){
 assert(locked && w->event_count && w->event_count<=64);
 os_window_event_t *last=&w->events[(w->event_write+63)%64];
 assert(last->identifier==w->identifier && last->type<=OS_WINDOW_EVENT_MOVED);
 ++wakes;
}
''' + pointer + producers + dispatch + r'''
static os_window_event_t expected[64];static unsigned count,expected_wakes;
static int32_t clamp(__int128 value){return value<INT32_MIN?INT32_MIN:value>INT32_MAX?INT32_MAX:(int32_t)value;}
static void oracle(window_server_window_t *w,unsigned op,const input_event_t *in){
 os_window_event_t e={.identifier=w->identifier};
 if(op==5){
  e.type=OS_WINDOW_EVENT_MOVED;
  e.moved.x=clamp((__int128)w->x+window_client_offset_x(w->flags));
  e.moved.y=clamp((__int128)w->y+window_client_offset_y(w->flags));
  if(count && expected[count-1].identifier==w->identifier && expected[count-1].type==OS_WINDOW_EVENT_MOVED){
   expected[count-1].moved=e.moved;return;
  }
 }else if(op==4){
  for(unsigned i=0;i<count;++i)
   if(expected[i].identifier==w->identifier && expected[i].type==OS_WINDOW_EVENT_CLOSE_REQUEST)return;
  e.type=OS_WINDOW_EVENT_CLOSE_REQUEST;
 }else if(op==3){e.type=OS_WINDOW_EVENT_MINIMIZED;e.minimized.minimized=w->minimized;
 }else if(op==2){
  e.type=OS_WINDOW_EVENT_RESIZE;e.resize.width=w->width;e.resize.height=w->height;e.resize.buffer_size=w->buffer_size;
  if(count && expected[count-1].identifier==w->identifier && expected[count-1].type==OS_WINDOW_EVENT_RESIZE){
   expected[count-1].resize=e.resize;return;
  }
 }else{
  e.type=OS_WINDOW_EVENT_INPUT;e.input=*in;
  e.pointer_x=clamp((__int128)g_window_server.pointer_x-w->x-window_client_offset_x(w->flags));
  e.pointer_y=clamp((__int128)g_window_server.pointer_y-w->y-window_client_offset_y(w->flags));
 }
 if(count==64){memmove(expected,expected+1,63*sizeof(*expected));--count;}
 expected[count++]=e;++expected_wakes;
}
static void apply(window_server_window_t *w,unsigned op,const input_event_t *in){
 oracle(w,op,in);
 if(op==1){registered=w;assert(window_server_dispatch(w->identifier,in)==K_OK);}
 else{
  window_lock();
  switch(op){
   case 0:window_enqueue_event_locked(w,in);break;
   case 2:window_enqueue_resize_event_locked(w);break;
   case 3:window_enqueue_minimized_event_locked(w);break;
   case 4:window_enqueue_close_request_locked(w);break;
   case 5:window_enqueue_moved_event_locked(w);break;
  }
  window_unlock();
 }
 assert(w->event_count==count && w->event_write==(w->event_read+count)%64 && wakes==expected_wakes);
 assert(w->canary_before==0x123456abcdefULL && w->canary_after==0xfedcba654321ULL);
 for(unsigned i=0;i<count;++i)assert(!memcmp(&w->events[(w->event_read+i)%64],&expected[i],sizeof(*expected)));
}
static void setup(window_server_window_t *w,unsigned read,unsigned depth,unsigned pattern){
 memset(w,0xa5,sizeof(*w));w->identifier=17;w->owner=w;
 w->flags=pattern&1?OS_WINDOW_CLIENT_DECORATIONS:0;
 w->width=945;w->height=1060;w->buffer_size=0xabcdef987ULL;
 w->x=pattern&2?INT32_MIN:10;w->y=pattern&4?INT32_MAX:20;w->minimized=pattern&1;
 w->event_read=read;w->event_write=(read+depth)%64;w->event_count=depth;
 w->canary_before=0x123456abcdefULL;w->canary_after=0xfedcba654321ULL;
 count=depth;wakes=expected_wakes=0;
 for(unsigned i=0;i<depth;++i){
  os_window_event_t e={.identifier=pattern==7?18:17,.type=(i+pattern)%4};
  e.input.timestamp=100+i;e.pointer_x=-321;e.pointer_y=654;
  expected[i]=e;w->events[(read+i)%64]=e;
 }
 g_window_server.pointer_x=pattern&2?UINT32_MAX:100;
 g_window_server.pointer_y=pattern&4?0:200;
}
int main(void){
 _Static_assert(sizeof(os_window_event_t)==40,"event ABI");
 window_server_window_t w;input_event_t input={.timestamp=123456,.device_id=5,.type=OS_INPUT_EVENT_BUTTON,.flags=3,.code=OS_INPUT_BUTTON_LEFT,.value=1};
 unsigned cases=0;
 for(unsigned read=0;read<64;++read)for(unsigned depth=0;depth<=64;++depth)
 for(unsigned pattern=0;pattern<8;++pattern)for(unsigned op=0;op<6;++op){
  setup(&w,read,depth,pattern);apply(&w,op,&input);++cases;
  // Exercise repeated close deduplication and resize coalescing after the first append.
  if(op==2 || op==4 || op==5){w.x=INT32_MAX;w.y=INT32_MIN;apply(&w,op,&input);++cases;}
 }
 setup(&w,63,0,0);uint32_t random=0xabcdef;
 for(unsigned i=0;i<200000;++i){
  random=random*1664525U+1013904223U;
  w.width=random;w.height=~random;w.buffer_size=(uint64_t)random<<32|i;
  w.minimized=i&1;input.timestamp=i+1;
  w.x=(int32_t)random;w.y=(int32_t)~random;
  apply(&w,random%6,&input);++cases;
  if(i%7==0 && count){w.event_read=(w.event_read+1)%64;--w.event_count;memmove(expected,expected+1,(count-1)*sizeof(*expected));--count;}
 }
 // Invalid public dispatch never modifies the queue or publishes a wake.
 registered=&w;unsigned before=wakes;
 assert(window_server_dispatch(w.identifier,NULL)==K_EINVAL);
 assert(window_server_dispatch(999,&input)==K_ENOENT);
 w.owner=NULL;assert(window_server_dispatch(w.identifier,&input)==K_ENOENT);w.owner=&w;
 initialized=false;assert(window_server_dispatch(w.identifier,&input)==K_EINVAL);initialized=true;
 assert(wakes==before && global_wakes==flushes);
 window_lock();window_enqueue_event_locked(NULL,&input);window_enqueue_event_locked(&w,NULL);
 window_enqueue_client_input_locked(NULL,&input);window_enqueue_client_input_locked(&w,NULL);
 window_enqueue_resize_event_locked(NULL);window_enqueue_close_request_locked(NULL);window_enqueue_minimized_event_locked(NULL);window_unlock();
 window_lock();window_enqueue_moved_event_locked(NULL);window_unlock();
 assert(wakes==before);
 printf("Window queue: %u boundary/mixed FIFO comparisons; wrap, eviction, payload clearing, pointer coordinates, close deduplication, resize/move coalescing and saturated client origins passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-window-queue-') as directory:
    path = Path(directory)
    def run(name, code, fail=False):
        (path/(name+'.c')).write_text(code)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        '-I'+str(root/'include'), str(path/(name+'.c')), '-o', str(path/name)], check=True)
        def no_core():
            resource.setrlimit(resource.RLIMIT_CORE,(0,0))
        result = subprocess.run([str(path/name)], text=True, capture_output=True, preexec_fn=no_core)
        if fail:
            assert result.returncode != 0 and 'Assertion' in result.stderr, result.stderr
            print('Negative control rejected: '+name)
        else:
            assert result.returncode == 0, result.stderr
            print(result.stdout.strip())
    run('production', program)
    if args.negative_controls:
        run('uncleared-slot', program.replace('*queued = (os_window_event_t){0};', '(void)queued;'), True)
        run('duplicate-close', program.replace('queued->type == OS_WINDOW_EVENT_CLOSE_REQUEST) return;',
                                             'queued->type == OS_WINDOW_EVENT_CLOSE_REQUEST) break;'), True)
        run('missing-pointer-snapshot', program.replace('queued->input = *event;\n    window_event_set_pointer_locked(window, queued);',
                                                       'queued->input = *event;'), True)
