set pagination off
target remote /tmp/system-waits.sock
thread apply all bt 8
set $registry = &g_process_registry
set $cpu_states = &g_cpus
set $cpu_features = &x86_boot_cpu_features
add-symbol-file build/process-inspection-types.o 0
add-symbol-file build/wait-inspection-types.o 0
add-symbol-file build/scheduler-inspection-types.o 0
add-symbol-file build/cpu-inspection-types.o 0
python
import gdb
import subprocess
import os
libc_symbols={line.split()[-1]:int(line.split()[0],16)
              for line in subprocess.check_output(
                  ['nm','-n','build/esp/lib/libsystemc.so.1'],text=True).splitlines()
              if len(line.split())==3 and line.split()[0]!='U'}
def offset(kind, field):
    return int(gdb.parse_and_eval('(unsigned long)&((%s*)0)->%s' % (kind,field)))
def cast(address, kind):
    return gdb.Value(address).cast(gdb.lookup_type(kind).pointer()).dereference()
def user_word(process, address):
    table=int(process['vm'].dereference()['root_table']['value'])
    mask=0x000ffffffffff000
    for shift in (39,30,21,12):
        pte=int(gdb.Value(0xffff800000000000+table+((address>>shift)&511)*8).cast(gdb.lookup_type('unsigned long').pointer()).dereference())
        if not pte & 1:
            raise ValueError('unmapped user address')
        if shift in (30,21) and pte & 128:
            physical=(pte & mask & ~((1<<shift)-1))+(address & ((1<<shift)-1))
            break
        table=pte & mask
    else:
        physical=table+(address & 4095)
    return int(gdb.Value(0xffff800000000000+physical).cast(gdb.lookup_type('unsigned long').pointer()).dereference())
registry=int(gdb.parse_and_eval('$registry'))
cpu_states=int(gdb.parse_and_eval('$cpu_states'))
hz=int(cast(int(gdb.parse_and_eval('$cpu_features')),'x86_cpu_features_t')['tsc_hz'])
clocks=[]
for cpu in range(4):
    state=cast(cpu_states+cpu*gdb.lookup_type('scheduler_cpu_t').sizeof,'scheduler_cpu_t')
    clock=int(state['queue']['clock_ns'])
    clocks.append(clock)
    print('cpu=%d clock_ns=%d runnable=%d current=%#x' %
          (cpu,clock,int(state['queue']['nr_running']),int(state['queue']['current'])))
approx_tsc=max(clocks)*hz//1000000000
print('last_tick_tsc_estimate=%#x hz=%d (not a live rdtsc)' % (approx_tsc,hz))
node=int(cast(registry,'list_head_t')['next'])
seen=set()
while node != registry and node not in seen:
    seen.add(node)
    process=cast(node-offset('process_t','global_node'),'process_t')
    pid=int(process['pid'])
    name=process['name'].string()
    if 'chrome' in name:
        if pid == 125 and os.environ.get('OS_INSPECT_TEXTFIELD'):
            # Verified from GetReadOnly/GetText in the current Chrome build.
            field=int(os.environ['OS_INSPECT_TEXTFIELD'],0)
            readonly=user_word(process,field+0x37c)&255
            model=user_word(process,field+0x368)
            render=user_word(process,model+0x10)
            tag=(user_word(process,render+0x28)>>56)&255
            length=user_word(process,render+0x20) if tag&128 else tag
            data=user_word(process,render+0x18) if tag&128 else render+0x18
            raw=b''.join(user_word(process,data+i).to_bytes(8,'little')
                         for i in range(0,min(length,128)*2,8))
            print('TEXTFIELD field=%#x readonly=%d length=%d text=%r' %
                  (field,readonly,length,raw[:min(length,128)*2].decode('utf-16le',errors='replace')))
        if pid == 125 and os.environ.get('OS_INSPECT_WINDOW'):
            # Offsets verified from current native window constructor and
            # MaybeIssueFrame disassembly. Revalidate after Chrome rebuilds.
            window=int(os.environ['OS_INSPECT_WINDOW'],0)
            identifier=user_word(process,window+0x98)&0xffffffff
            frame_source=user_word(process,window+0x90)
            flags=user_word(process,frame_source+0x88)
            print('FRAME_SOURCE window=%#x id=%d source=%#x delegate=%#x needs=%d in_flight=%d last_time=%d interval=%d' %
                  (window,identifier,frame_source,user_word(process,frame_source+0x10),
                   flags&255,(flags>>8)&255,user_word(process,frame_source+0x78),
                   user_word(process,frame_source+0x80)))
            if os.environ.get('OS_CAPTURE_SURFACE'):
                state=user_word(process,window+0x80)
                pixels=user_word(process,state+8)
                size=user_word(process,state+24)
                width,height=size&0xffffffff,size>>32
                assert 1 <= width <= 4096 and 1 <= height <= 4096
                rows=min(height,200)
                captured=bytearray()
                address=pixels
                end=pixels+width*rows*4
                inferior=gdb.selected_inferior()
                mask=0x000ffffffffff000
                while address < end:
                    table=int(process['vm']['root_table']['value'])
                    for shift in (39,30,21,12):
                        pte=int.from_bytes(bytes(inferior.read_memory(
                            0xffff800000000000+table+((address>>shift)&511)*8,8)),'little')
                        assert pte&1, 'unmapped shared surface'
                        if shift in (30,21) and pte&128:
                            physical=(pte&mask&~((1<<shift)-1))+(address&((1<<shift)-1))
                            break
                        table=pte&mask
                    else:
                        physical=table+(address&4095)
                    length=min(4096-(address&4095),end-address)
                    captured.extend(bytes(inferior.read_memory(0xffff800000000000+physical,length)))
                    address+=length
                with open('build/chrome-shared-surface.bgra','wb') as artifact:
                    artifact.write(captured)
                print('SURFACE_CAPTURE widget=%d pixels=%#x width=%d rows=%d bytes=%d' %
                      (user_word(process,state)&0xffffffff,pixels,width,rows,len(captured)))
        try:
            print('  libc_key_lock=%#x mutex_metadata_lock=%#x' %
                  (user_word(process,0x60000000+libc_symbols['g_key_lock'])&0xffffffff,
                   user_word(process,0x60000000+libc_symbols['g_mutex_metadata_lock'])&0xffffffff))
        except (gdb.error,ValueError):
            pass
        head=int(process['threads'].address)
        link=int(process['threads']['next'])
        visited=set()
        while link != head and link not in visited:
            visited.add(link)
            thread=cast(link-offset('thread_t','process_node'),'thread_t')
            waiter_address=int(thread['blocked_waiter'])
            text='pid=%d name=%s tid=%d state=%d cpu=%d class=%d prio=%d fs=%#x' % (pid,name,int(thread['tid']),int(thread['state']),int(thread['owner_cpu']),int(thread['sched_class']),int(thread['rt_priority']),int(thread['arch']['fs_base']))
            if waiter_address:
                waiter=cast(waiter_address,'waiter_t')
                text+=' waiter_state=%d deadline=%#x subscriptions=%d' % (int(waiter['state']),int(waiter['deadline_tsc']),int(waiter['subscription_count']))
                deadline=int(waiter['deadline_tsc'])
                if deadline != 0xffffffffffffffff and hz:
                    text+=' deadline_minus_last_tick_ms=%.3f' % ((deadline-approx_tsc)*1000/hz)
                sources=[]
                for i in range(min(int(waiter['subscription_count']),8)):
                    queue=int(waiter['subscriptions'][i]['queue'])
                    symbol=gdb.execute('info symbol %#x' % queue,to_string=True).strip()
                    try:
                        header=cast(queue-offset('object_header_t','signal_waiters'),'object_header_t')
                        ops=int(header['ops'])
                        if 0xffffffff80000000 <= ops < 0xffffffff90000000:
                            symbol='object:'+header['ops'].dereference()['type_name'].string()
                    except gdb.error:
                        pass
                    sources.append('%#x:%s' % (queue,symbol))
                text+=' queues='+str(sources)
            print(text)
            # A blocked syscall keeps its entry frame at the kernel stack top.
            # Read it without changing CR3 or the stopped CPU's registers.
            try:
                frame=cast(int(thread['kernel_stack_top'])-176,'arch_trap_frame_t')
                if (int(frame['cs']) == 0x23 and int(thread['state']) == 1
                        and 0 <= int(frame['vector']) < 0x100):
                    print('  runnable_trap vector=%#x rip=%#x rsp=%#x rdi=%#x rsi=%#x rax=%#x' %
                          tuple(int(frame[k]) for k in ('vector','rip','rsp','rdi','rsi','rax')))
                if int(frame['cs']) == 0x23 and int(frame['vector']) == 0x100:
                    print('  syscall=%d rip=%#x rsp=%#x rbp=%#x args=%s' %
                          (int(frame['rax']),int(frame['rip']),int(frame['rsp']),
                           int(frame['rbp']),','.join('%#x' % int(frame[k])
                           for k in ('rdi','rsi','rdx','r10','r8','r9'))))
                    if len(visited) == 1 or pid == 125:
                        if pid == 125:
                            fp=int(frame['rbp'])
                            chain=[]
                            try:
                                for depth in range(24):
                                    if not 0 < fp < 0x800000000000:
                                        break
                                    chain.append(user_word(process,fp+8))
                                    next_fp=user_word(process,fp)
                                    if next_fp <= fp:
                                        break
                                    fp=next_fp
                            except (gdb.error,ValueError):
                                pass
                            print('  user_frame_chain='+','.join('%#x' % address for address in chain))
                        sp=int(frame['rsp'])
                        try:
                            print('  main_user_stack='+','.join('%#x' % user_word(process,sp+i*8) for i in range(128)))
                        except (gdb.error, ValueError) as error:
                            print('  user_stack_unavailable='+str(error))
            except gdb.error:
                pass
            link=int(cast(link,'list_head_t')['next'])
    node=int(cast(node,'list_head_t')['next'])
end
detach
quit
