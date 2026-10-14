set pagination off
target remote /tmp/system-waits.sock
info threads
thread apply all info registers rip rsp eflags cr3
thread apply all bt 16
set $cpu_address = &g_cpus
add-symbol-file build/scheduler-inspection-types.o 0
python
import gdb
cpu_type=gdb.lookup_type('scheduler_cpu_t')
address=int(gdb.parse_and_eval('$cpu_address'))
for index in range(4):
    cpu=gdb.Value(address+index*cpu_type.sizeof).cast(cpu_type.pointer()).dereference()
    print('CPU_STATE %d %s' % (index,cpu))
end
detach
quit
