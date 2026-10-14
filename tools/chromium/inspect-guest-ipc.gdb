set pagination off
target remote /tmp/system-ipc.sock
set $registry_address = &g_process_registry
add-symbol-file build/linux-ipc-inspection-types.o 0
add-symbol-file build/pipe-inspection-types.o 0
python
import gdb
def cast(address, kind):
    return gdb.Value(int(address)).cast(gdb.lookup_type(kind).pointer()).dereference()
def pipe_state(endpoint):
    ep=cast(endpoint,'pipe_endpoint_t')
    return cast(ep['state'],'pipe_state_t')
def readiness(kind, data):
    if not int(data): return 'null'
    if kind == 7:
        ep=cast(data,'pipe_endpoint_t'); state=pipe_state(data)
        return 'pipe bytes=%d read_end=%d readers=%d writers=%d' % (
            int(state['count']),int(ep['read_end']),int(state['readers']),int(state['writers']))
    if kind == 9:
        ev=cast(data,'linux_eventfd_object_t')
        return 'eventfd counter=%d' % int(ev['counter'])
    if kind == 10:
        sock=cast(data,'linux_unix_socket_t')
        read=pipe_state(sock['read_end']) if int(sock['read_end']) else None
        channel=cast(sock['read_channel'],'linux_unix_channel_t') if int(sock['read_channel']) else None
        return 'unix rx_bytes=%d rx_pipe=%#x records=%d read_offset=%d write_offset=%d shutdown=%d' % (
            int(read['count']) if read is not None else -1,
            int(sock['read_end']),int(channel['count']) if channel is not None else -1,
            int(channel['bytes_read']) if channel is not None else -1,
            int(channel['bytes_written']) if channel is not None else -1,
            int(sock['read_shutdown']))
    return ''
registry=int(gdb.parse_and_eval('$registry_address'))
offset=int(gdb.parse_and_eval('(unsigned long)&((process_t*)0)->global_node'))
node=int(cast(registry,'list_head_t')['next']); seen=set()
while node != registry and node not in seen:
    seen.add(node); process=cast(node-offset,'process_t')
    if 'chrome' in process['name'].string():
        entries=process['linux_fds']['entries']
        print('IPC_PROCESS pid=%d' % int(process['pid']))
        for fd in range(entries.type.range()[1]+1):
            entry=entries[fd]; kind=int(entry['kind']); data=int(entry['private_data'])
            if kind in (7,9,10):
                print(' FD %d kind=%d object=%#x %s' % (fd,kind,data,readiness(kind,data)))
            if kind == 8 and data:
                epoll=cast(data,'linux_epoll_object_t')
                print(' EPOLL fd=%d object=%#x limit=%d sequence=%d' % (
                    fd,data,int(epoll['watch_limit']),int(epoll['control_sequence'])))
                for index in range(min(int(epoll['watch_limit']),256)):
                    watch=epoll['watches'][index]
                    if int(watch['used']):
                        print('  WATCH fd=%d kind=%d events=%#x object=%#x %s' % (
                            int(watch['descriptor']),int(watch['kind']),int(watch['events']),
                            int(watch['private_data']),readiness(int(watch['kind']),watch['private_data'])))
    node=int(cast(node,'list_head_t')['next'])
end
detach
quit
