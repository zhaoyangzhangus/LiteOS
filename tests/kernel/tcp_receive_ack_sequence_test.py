"""Exercise production receive-reply initialization with distinct TCP spaces."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/net/socket_protocol.c').read_text()
blocks = []
for name in ('socket_inject_tcp_ipv4_reply', 'socket_inject_tcp_ipv6_reply'):
    body = source[source.index('kstatus_t ' + name):]
    block = re.search(r'if \(reply != 0\) \{\s*reply->valid = true;.*?\n    \}', body, re.S)
    assert block, name
    blocks.append(block.group())
program = '''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define NET_TCP_FLAG_ACK 16
#define SOCKET_STREAM_BUFFER_SIZE 8192
typedef struct { uint32_t tcp_next_sequence, tcp_send_next_sequence; } state_t;
typedef struct { bool valid; uint32_t flags, sequence, acknowledgement, window; } reply_t;
'''
for index, block in enumerate(blocks):
    program += f'void init{index}(state_t *private, reply_t *reply, uint32_t expected, uint32_t count) {{\n{block}\n}}\n'
program += '''
int main(void) {
    state_t state = {90628547U, 549506188U};
    reply_t reply = {0};
    init0(&state, &reply, state.tcp_next_sequence, 1440);
    assert(reply.valid && reply.flags == NET_TCP_FLAG_ACK);
    assert(reply.sequence == state.tcp_send_next_sequence);
    assert(reply.acknowledgement == state.tcp_next_sequence);
    assert(reply.window == 6752);
    init1(&state, &reply, state.tcp_next_sequence, 8192);
    assert(reply.sequence == state.tcp_send_next_sequence);
    assert(reply.acknowledgement == state.tcp_next_sequence);
    assert(reply.window == 0);
    init0(&state, 0, 0, 0);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-tcp-ack-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('IPv4/IPv6 receive ACK: local send sequence, remote ACK and window passed')
