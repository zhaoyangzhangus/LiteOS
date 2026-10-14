"""Execute actual player event/clock wait helpers under deterministic I/O."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'tools/nvidia/vulkan_video_player.c').read_text()
helpers = source[source.index('static int window_closed('):source.index('static linux_window_create_t *video_window;')]
code = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>
#include "video_pacing.h"
typedef struct {unsigned type,payload[7];} os_window_event_t;
#define OS_WINDOW_EVENT_CLOSE_REQUEST 2U
#define OS_WINDOW_EVENT_RESIZE 3U
static int handle_window_event(const os_window_event_t *event){(void)event;return 0;}
static uint64_t now;
static unsigned mode,polls,reads,clock_calls;
static int last_timeout;
static int mock_clock_gettime(clockid_t id,struct timespec *out) {
    assert(id==CLOCK_MONOTONIC);++clock_calls;
    if(mode==9){errno=EIO;return -1;}
    out->tv_sec=now/1000000000;out->tv_nsec=now%1000000000;return 0;
}
static int mock_poll(struct pollfd *p,nfds_t count,int timeout) {
    assert(count==1&&p->fd==42&&p->events==POLLIN&&timeout>=0&&polls<1000);
    ++polls;last_timeout=timeout;p->revents=0;
    if(mode==0){now+=(uint64_t)timeout*1000000;return 0;}
    if(mode==1||mode==2) {
        now+=1000000;
        if(mode==2){errno=EINTR;return -1;}
        p->revents=POLLIN;return 1;
    }
    now+=1000000;
    if(mode==3||mode==6||mode==7||mode==8||mode==10||mode==11){p->revents=POLLIN;return 1;}
    if(mode==4){p->revents=POLLHUP;return 1;}
    errno=EIO;return -1;
}
static ssize_t mock_read(int fd,void *out,size_t size) {
    assert(fd==42&&size==sizeof(os_window_event_t));++reads;
    if(mode==6)return 0;
    if(mode==7)return size-1;
    if(mode==8||mode==10||mode==11){errno=mode==8?EIO:(mode==10?EAGAIN:EINTR);return -1;}
    os_window_event_t *event=out;
    event->type=mode==3?OS_WINDOW_EVENT_CLOSE_REQUEST:1;return size;
}
#define clock_gettime mock_clock_gettime
#define poll mock_poll
#define read mock_read
''' + helpers + r'''
static void reset(unsigned selected){mode=selected;now=1000000000;polls=reads=clock_calls=0;last_timeout=-1;}
int main(void) {
    for(unsigned selected=0;selected<12;++selected) {
        reset(selected);video_pacing_t pacing;video_pacing_init(&pacing,30,1);
        int result=wait_next_frame(42,&pacing);
        if(selected==0||selected==1||selected==2||selected==10||selected==11) {
            assert(result==0&&now>=pacing.deadline);
            assert(now-pacing.deadline<1000000&&pacing.deadline==1033333333);
            assert(polls==(selected==0?1U:34U));
        } else if(selected==3||selected==4||selected==6) {
            assert(result==1&&polls==1&&now<pacing.deadline);
        } else {
            assert(result==-1&&polls==(selected==9?0U:1U));
        }
    }
    reset(0);video_pacing_t pacing;video_pacing_init(&pacing,30,1);
    for(unsigned frame=0;frame<1000;++frame) {
        polls=0;now+=2000000; /* simulated decode time */
        assert(!wait_next_frame(42,&pacing));
        assert(now>=pacing.deadline&&now-pacing.deadline<1000000&&last_timeout<=34);
    }
}
'''
with tempfile.TemporaryDirectory(prefix='video-wait-') as directory:
    base = Path(directory)
    (base / 'test.c').write_text(code)
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I' + str(root / 'tools/nvidia'), str(base / 'test.c'),
                    '-o', str(base / 'test')], check=True)
    subprocess.run([str(base / 'test')], check=True, timeout=15)
print('Player wait: actual clock/poll/read paths, input and EINTR deadlines, close/HUP/EOF, short reads, I/O failures and 1000 decode intervals passed')
