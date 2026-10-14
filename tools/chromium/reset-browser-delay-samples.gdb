set pagination off
target remote /tmp/system-keys.sock
python
import gdb

inferior = gdb.selected_inferior()
for records, guard, size in (
    ('g_exec_load_delays', 'g_exec_load_delay_guard', 64 * 6 * 8),
    ('g_sched_wake_delays', 'g_sched_wake_trace_guard', 16 * 12 * 8),
    ('g_syscall_delays', 'g_syscall_delay_guard', 64 * 7 * 8),
):
    address = int(gdb.parse_and_eval('(unsigned long long)&' + guard))
    if inferior.read_memory(address, 1).tobytes() != b'\x00':
        raise gdb.GdbError('Cannot reset samples while publisher owns ' + guard)
    address = int(gdb.parse_and_eval('(unsigned long long)&' + records))
    inferior.write_memory(address, bytes(size))
    print('Reset ' + records)
end
detach
quit
