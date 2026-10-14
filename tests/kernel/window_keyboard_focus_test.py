"""Exercise the actual keyboard fallback with an inactive popup above its owner."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/graphics/scene.c').read_text()
helper = source[source.index('window_server_window_t *window_scene_keyboard_locked('):]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define OS_WINDOW_VISIBLE 1U
typedef struct { uint32_t identifier, flags; bool minimized, client_show_inactive; } window_server_window_t;
static struct { uint32_t focused_identifier, count; window_server_window_t *windows[3]; } g_window_server;
static window_server_window_t *window_scene_find_locked(uint32_t id) {
 for (unsigned i=0;i<g_window_server.count;i++)
  if(g_window_server.windows[i]->identifier==id) return g_window_server.windows[i];
 return 0;
}
static void window_scene_set_focus_identifier_locked(uint32_t id) {g_window_server.focused_identifier=id;}
''' + helper + r'''
int main(void) {
 window_server_window_t owner={.identifier=3,.flags=OS_WINDOW_VISIBLE};
 window_server_window_t popup={.identifier=8,.flags=OS_WINDOW_VISIBLE,.client_show_inactive=true};
 g_window_server.count=2;g_window_server.windows[0]=&owner;g_window_server.windows[1]=&popup;
 assert(window_scene_keyboard_locked()==&owner && g_window_server.focused_identifier==3);
 /* An explicit pointer activation may focus an interactive menu. */
 g_window_server.focused_identifier=8;
 assert(window_scene_keyboard_locked()==&popup);
 g_window_server.focused_identifier=0;owner.minimized=true;
 assert(window_scene_keyboard_locked()==0);
 owner.minimized=false;owner.flags=0;
 assert(window_scene_keyboard_locked()==0);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-keyboard-focus-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('Keyboard focus: inactive fallback excluded, explicit menu activation preserved')
