"""Execute NVIDIA poll/epoll readiness and actual delivery consumption branches."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/syscall/linux.c').read_text()
poll_start = source.index('    if (entry->kind == LINUX_FD_NVIDIA) {', source.index('static int16_t linux_poll_ready('))
poll = source[poll_start:source.index('    if (entry->kind == LINUX_FD_CONSOLE)', poll_start)]
epoll_start = source.index('    case LINUX_FD_NVIDIA: {', source.index('static bool linux_epoll_watch_ready('))
epoll = source[epoll_start:source.index('    case LINUX_FD_FILE:', epoll_start)]
supported = source[source.index('static bool linux_epoll_target_supported('):source.index('static void linux_epoll_watch_adopt(')]
consume_start = source.index('        if (watch->kind == LINUX_FD_NVIDIA &&', source.index('static int64_t linux_epoll_collect('))
consume_epoll = source[consume_start:source.index('#if OS_DEBUG_SERIAL', consume_start)]
consume_start = source.index('            if (entries[index].kind == LINUX_FD_NVIDIA &&', source.index('static int64_t linux_poll('))
consume_poll = source[consume_start:source.index('            if (pollfd->revents != 0)', consume_start)]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define LINUX_POLLIN 1U
#define LINUX_POLLPRI 2U
#define LINUX_POLLOUT 4U
#define LINUX_POLLHUP 16U
#define LINUX_EPOLLIN 1U
#define LINUX_EPOLLPRI 2U
#define LINUX_EPOLLHUP 16U
#define NV_FRONTEND_POLL_READ 1U
#define NV_FRONTEND_POLL_CLOSED 2U
enum {LINUX_FD_NVIDIA,LINUX_FD_FILE,LINUX_FD_PIPE,LINUX_FD_SOCKET,
      LINUX_FD_UNIX_SOCKET,LINUX_FD_WINDOW,LINUX_FD_EVENTFD};
typedef struct {unsigned data;bool dataless,closed;unsigned consumes;} frontend_t;
typedef struct {unsigned kind;void *private_data;} linux_fd_entry_t;
typedef struct {unsigned kind,events;void *private_data;} linux_epoll_watch_t;
typedef struct {int16_t revents;} linux_pollfd_t;
static bool linux_poll_read_requested(int16_t bits){return bits&(LINUX_POLLIN|LINUX_POLLPRI);}
static uint32_t nv_frontend_poll_state(void *p,bool consume) {
 frontend_t *f=p;assert(f);
 uint32_t state=(f->data||f->dataless?1U:0U)|(f->closed?2U:0U);
 if(consume){++f->consumes;f->dataless=false;}
 return state;
}
static int16_t poll_ready(const linux_fd_entry_t *entry,int16_t requested) {
 int16_t ready=0;
''' + poll + r'''
 return ready;
}
static uint32_t epoll_ready(linux_epoll_watch_t *watch) {
 uint32_t ready=0;bool read_requested=watch->events&(LINUX_EPOLLIN|LINUX_EPOLLPRI);
 switch(watch->kind) {
''' + epoll + r'''
 default:break;
 }
 return ready;
}
static void epoll_deliver(linux_epoll_watch_t *watch,uint32_t ready) {
''' + consume_epoll + r'''
}
static void poll_deliver(linux_fd_entry_t *entries,linux_pollfd_t *pollfd) {
 unsigned index=0;
''' + consume_poll + r'''
}
''' + supported + r'''
int main(void) {
 frontend_t f={.dataless=true};
 linux_fd_entry_t entry={LINUX_FD_NVIDIA,&f};
 linux_epoll_watch_t watch={LINUX_FD_NVIDIA,LINUX_EPOLLIN|LINUX_EPOLLPRI,&f};
 assert(linux_epoll_target_supported(&entry)&&!linux_epoll_target_supported(NULL));
 for(unsigned i=0;i<1000;++i) {
  assert(poll_ready(&entry,LINUX_POLLIN)==LINUX_POLLIN);
  assert(poll_ready(&entry,LINUX_POLLPRI)==LINUX_POLLPRI);
  assert(epoll_ready(&watch)==(LINUX_EPOLLIN|LINUX_EPOLLPRI));
 }
 assert(f.dataless&&!f.consumes); /* wait predicates do not swallow the event */
 epoll_deliver(&watch,epoll_ready(&watch));
 assert(!f.dataless&&f.consumes==1&&!epoll_ready(&watch));
 f.dataless=true;linux_pollfd_t result={.revents=poll_ready(&entry,LINUX_POLLIN)};
 poll_deliver(&entry,&result);assert(!f.dataless&&f.consumes==2);
 f.data=2;
 epoll_deliver(&watch,epoll_ready(&watch));assert(f.data==2&&epoll_ready(&watch));
 result.revents=poll_ready(&entry,LINUX_POLLPRI);poll_deliver(&entry,&result);assert(f.data==2);
 f.data=0;f.dataless=true;
 unsigned before=f.consumes;
 result.revents=poll_ready(&entry,LINUX_POLLOUT);poll_deliver(&entry,&result);
 assert(!result.revents&&f.dataless&&f.consumes==before);
 f.closed=true;f.dataless=false;
 assert(poll_ready(&entry,0)==LINUX_POLLHUP);
 watch.events=LINUX_EPOLLIN;assert(epoll_ready(&watch)==LINUX_EPOLLHUP);
 epoll_deliver(&watch,LINUX_EPOLLHUP);assert(f.consumes==before);
}
'''
with tempfile.TemporaryDirectory(prefix='nvidia-event-poll-') as directory:
    base = Path(directory)
    (base / 'test.c').write_text(code)
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(base / 'test.c'), '-o', str(base / 'test')], check=True)
    subprocess.run([str(base / 'test')], check=True, timeout=15)
print('NVIDIA poll/epoll: actual readiness/delivery branches, 1000 non-consuming predicates, PRI, dataless consumption, retained data and HUP passed')
