set pagination off
target remote /tmp/system-allocator.sock
set $cpu_registry = &g_cpus
add-symbol-file build/current-inspection-types.o 0
hbreak futex_wake_keyed
condition 1 $rsi == $allocator_lock && ((process_t*)$rdi)->pid == $browser_pid
commands 1
silent
python
import gdb,re
process = gdb.Value(int(gdb.parse_and_eval('$rdi'))).cast(gdb.lookup_type('process_t').pointer()).dereference()
cpus = gdb.Value(int(gdb.parse_and_eval('$cpu_registry'))).cast(gdb.lookup_type('scheduler_cpu_t').pointer())
match = re.search(r'CPU#(\d+)', gdb.selected_thread().name or '')
if not match:
    raise gdb.GdbError('Cannot establish QEMU CPU identity')
thread = cpus[int(match.group(1))]['queue']['current']
print('ALLOCATOR_ROOT_WAKE pid', int(process['pid']), 'tid', int(thread['tid']))
frame_type = gdb.lookup_type('arch_trap_frame_t')
trap = gdb.Value(int(thread['kernel_stack_top'])-frame_type.sizeof).cast(frame_type.pointer()).dereference()
if int(trap['cs']) != 0x23:
    raise gdb.GdbError('Not a user syscall frame')
print('USER_TRAP rip', hex(int(trap['rip'])), 'rbp', hex(int(trap['rbp'])), 'rsp', hex(int(trap['rsp'])))
def word(address):
    table=int(process['vm'].dereference()['root_table']['value'])
    mask=0x000ffffffffff000
    for shift in (39,30,21,12):
        pte=int.from_bytes(bytes(gdb.selected_inferior().read_memory(0xffff800000000000+table+((address>>shift)&511)*8,8)),'little')
        if not pte&1: raise gdb.MemoryError('unmapped user frame')
        if shift in (30,21) and pte&128:
            physical=(pte&mask&~((1<<shift)-1))+(address&((1<<shift)-1));break
        table=pte&mask
    else: physical=table+(address&4095)
    return int.from_bytes(bytes(gdb.selected_inferior().read_memory(0xffff800000000000+physical,8)),'little')
frame=int(trap['rbp'])
for depth in range(24):
    if not 0x10000 <= frame < 0x800000000000: break
    try: nxt,ret=word(frame),word(frame+8)
    except gdb.MemoryError: break
    print('OWNER_FRAME',depth,hex(frame),hex(ret))
    if nxt<=frame or nxt-frame>0x100000: break
    frame=nxt
end
detach
quit
end
continue
