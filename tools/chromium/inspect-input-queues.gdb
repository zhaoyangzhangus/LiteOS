set pagination off
target remote /tmp/system-input.sock
set $input_address = &g_input
set $window_address = &g_window_server
set $work_address = &g_deferred
set $usb_queued = &g_xhci_work_queued
add-symbol-file build/input-inspection-types.o 0
add-symbol-file build/window-inspection-types.o 0
add-symbol-file build/deferred-inspection-types.o 0
python
import gdb
def anonymous(address, name):
    symbol=next(s for s in gdb.lookup_static_symbols(name) if s.type.code == gdb.TYPE_CODE_STRUCT)
    return gdb.Value(int(gdb.parse_and_eval(address))).cast(symbol.type.pointer()).dereference()
input_state=anonymous('$input_address','g_input')
work=anonymous('$work_address','g_deferred')
server=gdb.Value(int(gdb.parse_and_eval('$window_address'))).cast(gdb.lookup_type('window_server_state_t').pointer()).dereference()
print('input_count=%d pending=%d dropped=%d lock=%d' %
      (int(input_state['count']),int(input_state['pending']),int(input_state['dropped']),int(input_state['lock']['state'])))
print('usb_queued=%d work_count=%d critical_ready=%d worker_state=%d' %
      (int(gdb.parse_and_eval('*(unsigned char*)$usb_queued')),int(work['count']),int(work['critical_ready']),int(work['worker']['state'])))
print('focused_window=%d' % int(server['focused_identifier']))
print('pointer=(%d,%d)' % (int(server['pointer_x']),int(server['pointer_y'])))
low,high=server['windows'].type.range()
for index in range(low,high+1):
    pointer=server['windows'][index]
    if int(pointer):
        window=pointer.dereference()
        print('window=%d events=%d wake_pending=%d hidden=%d title=%s' %
              (int(window['identifier']),int(window['event_count']),int(window['event_wake_pending']),int(window['client_hidden']),window['title'].string()))
end
detach
quit
