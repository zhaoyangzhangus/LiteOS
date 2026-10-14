"""Execute production owner-checked, lock-consistent client geometry query."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root/'kernel/graphics/window.c').read_text()
begin = source.index('kstatus_t window_server_geometry_owned(')
body = source[begin:source.index('kstatus_t window_server_set_fullscreen_owned(', begin)]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <uapi/window.h>
typedef int process_t,kstatus_t;
enum {K_OK=0,K_EINVAL=-22,K_EPERM=-1};
typedef struct {process_t *owner;int32_t x,y;uint32_t width,height,flags;bool maximized,fullscreen;} window_server_window_t;
static bool locked,ready=true,registered=true;
static bool window_server_kernel_ready(void){return ready;}
static void window_lock(void){assert(!locked);locked=true;}
static void window_unlock(void){assert(locked);locked=false;}
static bool window_server_owner_locked(process_t *p,window_server_window_t *w){assert(locked);return registered&&p==w->owner;}
static uint32_t window_client_offset_x(uint32_t f){return f&OS_WINDOW_CLIENT_DECORATIONS?0:1;}
static uint32_t window_client_offset_y(uint32_t f){return f&OS_WINDOW_CLIENT_DECORATIONS?0:1+os_window_caption_height(f);}
''' + body + r'''
int main(void){
 process_t owner,other;int32_t x,y;uint32_t width,height,maximized;
 window_server_window_t w={.owner=&owner,.x=240,.y=135,.width=640,.height=360};
 for(unsigned caption=40;caption<=128;++caption)for(unsigned client=0;client<2;++client)for(unsigned max=0;max<2;++max){
  w.flags=OS_WINDOW_STANDARD_CAPTION_CONTROLS|OS_WINDOW_CAPTION_HEIGHT(caption)|(client?OS_WINDOW_CLIENT_DECORATIONS:0);
  w.maximized=max;
  assert(!window_server_geometry_owned(&owner,&w,&x,&y,&width,&height,&maximized));
  assert(x==240+(client?0:1)&&y==135+(int32_t)(client?0:caption+1)&&width==640&&height==360&&maximized==max&&!locked);
  w.fullscreen=true;
  assert(!window_server_geometry_owned(&owner,&w,&x,&y,&width,&height,&maximized)&&maximized==(max|2));
  w.fullscreen=false;
 }
 x=-42;width=43;
 assert(window_server_geometry_owned(&other,&w,&x,&y,&width,&height,&maximized)==-1&&x==-42&&width==43&&!locked);
 registered=false;assert(window_server_geometry_owned(&owner,&w,&x,&y,&width,&height,&maximized)==-1);registered=true;
 ready=false;assert(window_server_geometry_owned(&owner,&w,&x,&y,&width,&height,&maximized)==-22);ready=true;
 assert(window_server_geometry_owned(&owner,&w,0,&y,&width,&height,&maximized)==-22);
 assert(window_server_geometry_owned(&owner,&w,&x,0,&width,&height,&maximized)==-22);
 assert(window_server_geometry_owned(&owner,&w,&x,&y,0,&height,&maximized)==-22);
 assert(window_server_geometry_owned(&owner,&w,&x,&y,&width,0,&maximized)==-22);
 assert(window_server_geometry_owned(&owner,&w,&x,&y,&width,&height,0)==-22);
}
'''
with tempfile.TemporaryDirectory(prefix='window-geometry-query-') as directory:
    base = Path(directory);(base/'test.c').write_text(code)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root/'include'),str(base/'test.c'),'-o',str(base/'test')],check=True)
    subprocess.run([str(base/'test')],check=True)
print('Window geometry query: owner isolation, 356 frame/state combinations and output guards passed')
