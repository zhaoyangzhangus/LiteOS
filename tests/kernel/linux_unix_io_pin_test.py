"""Production Unix directional lifetime helpers across shutdown and concurrent I/O.

Six syscall integration checks complement the existing actual timeout-call test;
this fixture tests object ownership, not ancillary payload encoding.
"""
from pathlib import Path
import re
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/syscall/linux.c').read_text()

def body(name):
    match = re.search(r'static [^\n]+ ' + name + r'\(', source)
    assert match, name
    start = source.index('{', match.start())
    depth, end = 1, start + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]

for name in ('linux_sendmsg', 'linux_recvmsg', 'linux_unix_sendto',
             'linux_unix_recvfrom', 'linux_read', 'linux_write'):
    text = body(name)
    assert 'linux_unix_socket_pin_io(' in text, name
    assert 'linux_unix_socket_unpin_io(' in text, name
    assert not re.search(r'pipe_(?:read|write)\(socket->', text), name
    assert not re.search(r'channel = socket->(?:read|write)_channel', text), name

code = r'''
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef int kstatus_t;
#define K_OK 0
#define K_EINVAL -22
#define KOBJECT_TYPE_LINUX_UNIX_SOCKET 77
#define OS_SOCKET_SHUT_READ 0
#define OS_SOCKET_SHUT_WRITE 1
#define OS_SOCKET_SHUT_BOTH 2
typedef struct { atomic_uint refs; } owned_t;
typedef owned_t pipe_endpoint_t;
typedef owned_t linux_unix_channel_t;
typedef struct {
 struct {unsigned type;} object;
 pthread_mutex_t lock;
 pipe_endpoint_t *read_end,*write_end;
 linux_unix_channel_t *read_channel,*write_channel;
 bool read_shutdown,write_shutdown;
} linux_unix_socket_t;
static _Thread_local unsigned locked;
static atomic_uint gets,put_calls,notifications;
static kstatus_t pipe_shutdown(pipe_endpoint_t *p) {
 assert(!locked && p && atomic_load(&p->refs));return K_OK;
}
static void spinlock_lock(pthread_mutex_t *lock) {assert(!locked);pthread_mutex_lock(lock);++locked;}
static void spinlock_unlock(pthread_mutex_t *lock) {assert(locked==1);--locked;pthread_mutex_unlock(lock);}
static void object_get(void *p) {assert(locked);assert(atomic_fetch_add(&((owned_t*)p)->refs,1));atomic_fetch_add(&gets,1);}
static void object_put(void *p) {assert(!locked);assert(atomic_fetch_sub(&((owned_t*)p)->refs,1));atomic_fetch_add(&put_calls,1);}
static void object_notify_changed(void *p) {(void)p;assert(!locked);atomic_fetch_add(&notifications,1);}
''' + '\n'.join(body(name) for name in (
    'linux_unix_socket_release_endpoint', 'linux_unix_socket_release_channel',
    'linux_unix_socket_shutdown', 'linux_unix_socket_pin_io',
    'linux_unix_socket_unpin_io')) + r'''
static linux_unix_socket_t socket_value;
static owned_t values[4];
static void initialize(bool channels) {
 socket_value=(linux_unix_socket_t){0};
 socket_value.object.type=KOBJECT_TYPE_LINUX_UNIX_SOCKET;
 assert(!pthread_mutex_init(&socket_value.lock,0));
 for(unsigned i=0;i<4;++i)atomic_store(&values[i].refs,i<2||channels?1:0);
 socket_value.read_end=&values[0];socket_value.write_end=&values[1];
 if(channels){socket_value.read_channel=&values[2];socket_value.write_channel=&values[3];}
}
static void finish(void) {
 assert(linux_unix_socket_shutdown(&socket_value,OS_SOCKET_SHUT_BOTH)==K_OK);
 for(unsigned i=0;i<4;++i)assert(!atomic_load(&values[i].refs));
 assert(!pthread_mutex_destroy(&socket_value.lock));
}
static void *worker(void *opaque) {
 bool write=(uintptr_t)opaque!=0;
 for(unsigned i=0;i<10000;++i) {
  pipe_endpoint_t *end=(void*)1;linux_unix_channel_t *channel=(void*)1;
  linux_unix_socket_pin_io(&socket_value,write,&end,&channel);
  if(end) {
   assert(end==&values[write?1:0] && atomic_load(&end->refs)>0);
   if(channel)assert(channel==&values[write?3:2] && atomic_load(&channel->refs)>0);
  } else assert(!channel);
  linux_unix_socket_unpin_io(end,channel);
 }
 return 0;
}
int main(void) {
 for(unsigned channels=0;channels<2;++channels)for(unsigned write=0;write<2;++write)
 for(unsigned how=0;how<3;++how)for(unsigned before=0;before<2;++before) {
  initialize(channels);pipe_endpoint_t *end;linux_unix_channel_t *channel;
  if(before)assert(linux_unix_socket_shutdown(&socket_value,how)==K_OK);
  linux_unix_socket_pin_io(&socket_value,write,&end,&channel);
  bool detached=how==2 || how==write;
  if(before && detached)assert(!end && !channel);
  else {
   assert(end==&values[write?1:0] && atomic_load(&end->refs)==2);
   if(channels)assert(channel==&values[write?3:2] && atomic_load(&channel->refs)==2);
   else assert(!channel);
  }
  if(!before)assert(linux_unix_socket_shutdown(&socket_value,how)==K_OK);
  if(end){assert(atomic_load(&end->refs)>0);if(channel)assert(atomic_load(&channel->refs)>0);}
  linux_unix_socket_unpin_io(end,channel);finish();
 }
 for(unsigned round=0;round<16;++round) {
  initialize(round&1);pthread_t workers[4];
  for(unsigned i=0;i<4;++i)assert(!pthread_create(&workers[i],0,worker,(void*)(uintptr_t)(i&1)));
  assert(linux_unix_socket_shutdown(&socket_value,round%3)==K_OK);
  for(unsigned i=0;i<4;++i)assert(!pthread_join(workers[i],0));
  finish();
 }
 assert(atomic_load(&put_calls)==atomic_load(&gets)+120);
 puts("Unix I/O pins: 24 directional shutdown cases, 640000 concurrent snapshots, endpoint/channel lifetime, balanced release and six syscall integrations passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-unix-io-pin-') as directory:
    binary = str(Path(directory)/'test')
    command = ['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
               '-pthread', '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
               '-x', 'c', '-', '-o', binary]
    subprocess.run(command, input=code, text=True, check=True)
    subprocess.run([binary], check=True, timeout=30)
    negative = code.replace('if (*channel != 0) object_get(*channel);', '')
    assert negative != code
    subprocess.run(command, input=negative, text=True, check=True)
    result = subprocess.run([binary], capture_output=True, text=True, timeout=30,
                            preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
    assert result.returncode != 0 and 'Assertion' in result.stderr
    print('Negative control rejected: endpoint retained without its channel')
