"""Exercise actual keepalive options, transport and IPv4/IPv6 ACK handlers."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
def function(file,signature):
    text=(root/file).read_text()
    start=text.index(signature)
    end=text.index('{',start)+1
    depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}')
        end+=1
    return text[start:end]
model=(root/'kernel/net/socket_model.h').read_text()
private=model[model.index('typedef struct socket_private {'):model.index('} socket_private_t;')+len('} socket_private_t;')]
public=(root/'include/kernel/socket.h').read_text()
output_types=public[public.index('typedef struct socket_tcp_reply {'):public.index('/* UDP 输出')]
protocol=(root/'kernel/net/socket_protocol.c').read_text()
inheritance=[]
for family in ('4','6'):
    start=protocol.index('child_private = socket_private(child);',
                         protocol.index('kstatus_t socket_inject_tcp_syn_ipv'+family+'('))
    start=protocol.index('socket_lock(&listener->lock);',start)
    end=protocol.index('socket_unlock(&listener->lock);',start)+len('socket_unlock(&listener->lock);')
    inheritance.append(f'void inherit{family}(socket_t *listener, socket_private_t *listener_private, socket_private_t *child_private) {{\n'+protocol[start:end]+'\n}')
program=r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include "uapi/socket.h"
#define OS_DEBUG_SERIAL 0
#define K_OK 0
#define K_EINVAL -22
#define K_EIO -5
#define K_ENOENT -2
#define K_EAGAIN -11
#define KOBJECT_TYPE_SOCKET 1
#define SOCKET_PROTOCOL_TCP 6
#define SOCKET_TCP_CLOSED 0
#define SOCKET_TCP_SYN_RECEIVED 1
#define SOCKET_TCP_ESTABLISHED 2
#define SOCKET_TCP_SYN_SENT 3
#define SOCKET_BINDING_COUNT 1
#define SOCKET_MAX_PAYLOAD 1440
#define SOCKET_RX_QUEUE_DEPTH 4
#define SOCKET_STREAM_BUFFER_SIZE 8192
#define SOCKET_LISTEN_BACKLOG 4
#define SOCKET_TCP_MAX_RETRANSMITS 3
#define SOCKET_POLL_READ 1
#define SOCKET_POLL_HUP 2
#define SOCKET_POLL_ERROR 4
#define SOCKET_POLL_WRITE 8
#define NET_TCP_FLAG_ACK 16
#define NET_TCP_FLAG_SYN 2
#define NET_TCP_FLAG_FIN 1
typedef int kstatus_t;
typedef int spinlock_t;
typedef struct socket {
 struct {int type;} object;
 unsigned type,family,protocol;int lock,waitq,refs;void *protocol_state;
} socket_t;
typedef struct {int unused;} socket_datagram_t;
''' + private + output_types + r'''
typedef struct {socket_t *socket;} socket_tcp_connection_t;
typedef struct {socket_t *socket;} socket_tcp6_connection_t;
static socket_t sockets[2];static socket_private_t states[2];
static socket_tcp_connection_t conn4;static socket_tcp6_connection_t conn6;
static uint64_t now=1000000000ULL;
static int locked,wakes,notifications,outputs[2];
static bool fail_outputs, missing_outputs;
static uint32_t last_sequence[2],last_ack[2];
static uint64_t x86_read_tsc(void){return now;}
static uint64_t x86_timeout_ns_to_tsc(uint64_t ns){return ns;}
static uint64_t socket_tcp_retransmit_deadline(uint64_t tick){return tick+1000000000ULL;}
static void socket_initialize_globals(void){}
static void socket_lock(spinlock_t *lock){assert(!locked);(void)lock;locked=1;}
static void socket_unlock(spinlock_t *lock){assert(locked);(void)lock;locked=0;}
static int binding_lock;
static spinlock_t *socket_binding_lock(void){return &binding_lock;}
static const socket_tcp_connection_t *socket_tcp_connection_at(uint32_t i){assert(i==0);return &conn4;}
static const socket_tcp6_connection_t *socket_tcp6_connection_at(uint32_t i){assert(i==0);return &conn6;}
static int32_t socket_find_tcp_locked(uint32_t a,uint16_t b,uint32_t c,uint16_t d){assert(locked);(void)a;(void)b;(void)c;(void)d;return conn4.socket?0:-1;}
static int32_t socket_find_tcp6_locked(const uint8_t *a,uint16_t b,const uint8_t *c,uint16_t d){assert(locked);(void)a;(void)b;(void)c;(void)d;return conn6.socket?0:-1;}
static bool socket_address6_zero(const uint8_t *a){uint8_t v=0;for(int i=0;i<16;++i)v|=a[i];return !v;}
static socket_private_t *socket_private(socket_t *s){return s?s->protocol_state:0;}
static const socket_private_t *socket_private_const(const socket_t *s){return s?s->protocol_state:0;}
typedef struct { socket_private_t *private; } socket_wait_context_t;
static bool object_try_get(socket_t *s){++s->refs;return true;}
static void object_put(socket_t *s){assert(s->refs>0);--s->refs;}
static void object_notify_changed(socket_t *s){assert(!locked && s);++notifications;}
static void object_notify_signaled(socket_t *s){object_notify_changed(s);}
static int wake_all(int *q){assert(!locked && q);++wakes;return 0;}
static void socket_copy(void *a,const void *b,size_t n){memcpy(a,b,n);}
static void socket_zero(void *a,size_t n){memset(a,0,n);}
static kstatus_t output4(void *ctx,uint32_t src,uint16_t sp,uint32_t dst,uint16_t dp,
 uint32_t seq,uint32_t ack,uint8_t flags,uint16_t window,const void *payload,size_t length){
 (void)ctx;(void)payload;assert(!locked && src && dst && sp && dp);
 assert(flags==NET_TCP_FLAG_ACK && length==0 && window==8192);
 ++outputs[0];last_sequence[0]=seq;last_ack[0]=ack;return fail_outputs?K_EAGAIN:0;
}
static kstatus_t output6(void *ctx,const uint8_t *src,uint16_t sp,const uint8_t *dst,uint16_t dp,
 uint32_t seq,uint32_t ack,uint8_t flags,uint16_t window,const void *payload,size_t length){
 (void)src;(void)dst;int status=output4(ctx,1,sp,2,dp,seq,ack,flags,window,payload,length);
 --outputs[0];++outputs[1];last_sequence[1]=seq;last_ack[1]=ack;return status;
}
static void socket_tcp_output_get(socket_tcp_ipv4_output_fn *fn,void **ctx){*fn=missing_outputs?0:output4;*ctx=0;}
static void socket_tcp6_output_get(socket_tcp_ipv6_output_fn *fn,void **ctx){*fn=missing_outputs?0:output6;*ctx=0;}
''' + '\n'.join(function('kernel/net/socket.c',signature) for signature in (
    'static uint64_t socket_tcp_keepalive_deadline(',
    'void socket_tcp_keepalive_activity_locked(',
    'bool socket_tcp_keepalive_poll_locked(',
    'kstatus_t socket_get_option(', 'kstatus_t socket_set_option(',
    'static bool socket_is_signaled(',
    'uint32_t socket_poll_state(')) + '\n' + function('kernel/net/socket_io.c','static bool socket_receive_ready(') + '\n' + function('kernel/net/socket_transport.c','static void socket_tcp_keepalive_output_failed_locked(') + '\n' + function('kernel/net/socket_transport.c','void socket_tcp_poll(') + '\n' + '\n'.join(
    function('kernel/net/socket_protocol.c',f'kstatus_t socket_inject_tcp_ack_ipv{family}_reply(') for family in ('4','6')) + '\n' + '\n'.join(inheritance) + r'''
static void reset(void) {
 memset(states,0,sizeof(states));memset(sockets,0,sizeof(sockets));
 now=1000000000ULL;wakes=notifications=outputs[0]=outputs[1]=0;fail_outputs=false;missing_outputs=false;
 for(int i=0;i<2;++i){
  sockets[i]=(socket_t){.object={KOBJECT_TYPE_SOCKET},.family=i?OS_AF_INET6:OS_AF_INET4,
   .type=OS_SOCK_STREAM,.protocol=SOCKET_PROTOCOL_TCP,.protocol_state=&states[i]};
  states[i].tcp_state=SOCKET_TCP_ESTABLISHED;states[i].connected=true;
  states[i].tcp_send_next_sequence=1000;states[i].tcp_next_sequence=2000;
  states[i].tcp_keepidle=7200;states[i].tcp_keepintvl=75;states[i].tcp_keepcnt=9;
  states[i].local_address=1;states[i].peer_address=2;
  states[i].local_port=123;states[i].peer_port=456;
  states[i].tcp_peer_window=8192;
 }
 conn4.socket=&sockets[0];conn6.socket=&sockets[1];
}
static void configure(void){
 for(int i=0;i<2;++i){
  assert(socket_set_option(&sockets[i],OS_SOCKET_OPTION_KEEPALIVE,1)==0);
  assert(socket_set_option(&sockets[i],OS_SOCKET_OPTION_KEEPIDLE,2)==0);
  assert(socket_set_option(&sockets[i],OS_SOCKET_OPTION_KEEPINTVL,1)==0);
  assert(socket_set_option(&sockets[i],OS_SOCKET_OPTION_KEEPCNT,2)==0);
 }
}
int main(void){
 reset();int32_t value;
 assert(socket_get_option(&sockets[0],OS_SOCKET_OPTION_KEEPALIVE,&value)==0 && !value);
 assert(socket_get_option(&sockets[0],OS_SOCKET_OPTION_KEEPIDLE,&value)==0 && value==7200);
 assert(socket_get_option(&sockets[0],OS_SOCKET_OPTION_KEEPINTVL,&value)==0 && value==75);
 assert(socket_get_option(&sockets[0],OS_SOCKET_OPTION_KEEPCNT,&value)==0 && value==9);
 socket_tcp_poll(now+10000000000000ULL);assert(!outputs[0] && !outputs[1]);
 configure();
 assert(socket_set_option(&sockets[0],OS_SOCKET_OPTION_KEEPIDLE,0)==K_EINVAL);
 assert(socket_set_option(&sockets[0],OS_SOCKET_OPTION_KEEPIDLE,32768)==K_EINVAL);
 assert(socket_set_option(&sockets[0],OS_SOCKET_OPTION_KEEPCNT,128)==K_EINVAL);
 assert(socket_set_option(&sockets[0],OS_SOCKET_OPTION_KEEPCNT,-1)==K_EINVAL);
 socket_tcp_poll(now+1999999999ULL);assert(!outputs[0] && !outputs[1]);
 now+=2000000000ULL;socket_tcp_poll(now);
 for(int i=0;i<2;++i){assert(outputs[i]==1 && last_sequence[i]==999 && last_ack[i]==2000);assert(states[i].tcp_send_next_sequence==1000 && !states[i].tcp_send_pending);}
 // Valid peer ACK resets idle/probe budget. Wrong sequence/future ACK does not.
 uint8_t a[16]={1},b[16]={2};
 assert(socket_inject_tcp_ack_ipv4_reply(2,456,1,123,2001,1000,NET_TCP_FLAG_ACK,8192,0)==0);
 assert(states[0].tcp_keep_probes==1);
 assert(socket_inject_tcp_ack_ipv4_reply(2,456,1,123,2000,1001,NET_TCP_FLAG_ACK,8192,0)==0);
 assert(states[0].tcp_keep_probes==1);
 assert(socket_inject_tcp_ack_ipv4_reply(2,456,1,123,2000,1000,NET_TCP_FLAG_ACK,8192,0)==0);
 assert(socket_inject_tcp_ack_ipv6_reply(b,456,a,123,2000,1000,NET_TCP_FLAG_ACK,8192,0)==0);
 assert(!states[0].tcp_keep_probes && !states[1].tcp_keep_probes);
 socket_tcp_poll(now+1000000000ULL);assert(outputs[0]==1 && outputs[1]==1);
 now+=2000000000ULL;socket_tcp_poll(now);assert(outputs[0]==2 && outputs[1]==2);
 now+=1000000000ULL;socket_tcp_poll(now);assert(outputs[0]==3 && outputs[1]==3);
 now+=1000000000ULL;socket_tcp_poll(now);
 for(int i=0;i<2;++i){assert(atomic_load(&states[i].tcp_keep_failed));assert(socket_get_option(&sockets[i],OS_SOCKET_OPTION_ERROR,&value)==0 && value==110);assert(socket_poll_state(&sockets[i])&SOCKET_POLL_ERROR);assert(sockets[i].refs==0);}
 assert(wakes==2 && notifications==2);socket_tcp_poll(now+10000000000ULL);assert(wakes==2 && outputs[0]==3 && outputs[1]==3);
 assert(socket_is_signaled(&sockets[0]) && socket_is_signaled(&sockets[1]));
 socket_wait_context_t waiting={.private=&states[0]};
 assert(socket_receive_ready(&waiting));
 assert(!socket_receive_ready(0));waiting.private=0;assert(!socket_receive_ready(&waiting));
 reset();configure();
 // Disable, local peers, FIN and closed sockets must not be probed.
 socket_set_option(&sockets[0],OS_SOCKET_OPTION_KEEPALIVE,0);
 states[1].peer=&sockets[0];socket_tcp_poll(now+10000000000ULL);assert(!outputs[0] && !outputs[1]);
 states[1].peer=0;states[1].tcp_fin_sent=true;socket_tcp_poll(now+10000000000ULL);assert(!outputs[1]);
 states[1].tcp_fin_sent=false;atomic_store(&states[1].peer_closed,true);socket_tcp_poll(now+10000000000ULL);assert(!outputs[1]);
 reset();configure();states[0].tcp_send_pending=true;states[1].tcp_send_pending=true;
 socket_tcp_poll(now+10000000000ULL);assert(!outputs[0] && !outputs[1] && !states[0].tcp_keep_probes);
 reset();configure();states[0].tcp_send_next_sequence=0;states[1].tcp_send_next_sequence=0;
 socket_tcp_poll(now+2000000000ULL);assert(last_sequence[0]==UINT32_MAX && last_sequence[1]==UINT32_MAX);
 assert(socket_tcp_keepalive_deadline(UINT64_MAX-10,1)==UINT64_MAX);
 reset();configure();fail_outputs=true;
 now+=2000000000ULL;socket_tcp_poll(now);
 assert(outputs[0]==1 && outputs[1]==1 && !states[0].tcp_keep_probes && !states[1].tcp_keep_probes);
 assert(!atomic_load(&states[0].tcp_keep_failed) && !wakes);
 fail_outputs=false;now+=1000000000ULL;socket_tcp_poll(now);
 assert(outputs[0]==2 && outputs[1]==2 && states[0].tcp_keep_probes==1 && states[1].tcp_keep_probes==1);
 uint64_t old_deadline=states[0].tcp_keep_deadline_tsc;
 socket_tcp_keepalive_activity_locked(&states[0],now);
 socket_tcp_keepalive_output_failed_locked(&states[0],old_deadline,now);
 assert(!states[0].tcp_keep_probes && states[0].tcp_keep_deadline_tsc==now+2000000000ULL);
 reset();configure();missing_outputs=true;
 now+=2000000000ULL;socket_tcp_poll(now);
 assert(!outputs[0] && !outputs[1] && !states[0].tcp_keep_probes && !states[1].tcp_keep_probes);
 assert(!socket_is_signaled(&sockets[0]) && !socket_is_signaled(&sockets[1]));
 waiting.private=&states[0];assert(!socket_receive_ready(&waiting));
 missing_outputs=false;now+=1000000000ULL;socket_tcp_poll(now);
 assert(outputs[0]==1 && outputs[1]==1 && states[0].tcp_keep_probes==1 && states[1].tcp_keep_probes==1);
 reset();configure();
 states[1].tcp_keepidle=11;states[1].tcp_keepintvl=12;states[1].tcp_keepcnt=13;
 states[0].tcp_keepalive=false;
 inherit4(&sockets[1],&states[1],&states[0]);
 assert(states[0].tcp_keepalive && states[0].tcp_keepidle==11 && states[0].tcp_keepintvl==12 && states[0].tcp_keepcnt==13);
 states[0].tcp_keepidle=0;inherit6(&sockets[1],&states[1],&states[0]);assert(states[0].tcp_keepidle==11);
 sockets[0].type=OS_SOCK_DGRAM;
 assert(socket_set_option(&sockets[0],OS_SOCKET_OPTION_KEEPIDLE,1)==K_EINVAL);
 assert(socket_get_option(&sockets[0],OS_SOCKET_OPTION_KEEPIDLE,&value)==K_EINVAL);
 assert(!locked && sockets[0].refs==0 && sockets[1].refs==0);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-keepalive-') as directory:
    path=Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer','-no-pie','-I',str(root/'include'),
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('TCP keepalive: production IPv4/IPv6 probes, ACK reset, timeout/poll/object/receive wake, options, listener inheritance, missing output, TX backpressure, disable, pending sends, FIN, closure and wrap passed (ASan/UBSan)')
