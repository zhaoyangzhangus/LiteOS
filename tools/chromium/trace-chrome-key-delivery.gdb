set pagination off
set confirm off
python
import gdb, time, struct, sys
from pathlib import Path
sys.path.insert(0, str(Path('tools/chromium').resolve()))
from trace_build import validate_chrome_build
expected = 'Build ID: 5c734622f8e4d5fe'
try:
    validate_chrome_build(expected)
except RuntimeError as error:
    raise gdb.GdbError('Refusing stale Chrome diagnostic: ' + str(error))
end
target remote /tmp/system-keys.sock
python
# Keyboard diagnostic only, for synthetic Ctrl+T; never capture text. Stops
# perturb timing. Three hardware addresses, no software patches or BP deletion.
started = None
tab_started = None
browser_pid = None

def current():
    cpu = gdb.selected_thread().num-1
    if not 0 <= cpu < int(gdb.parse_and_eval('g_cpu_count')):
        raise gdb.GdbError('Unexpected vCPU number')
    return gdb.parse_and_eval('g_cpus[%d].queue.current' % cpu).dereference()

class KeyEntry(gdb.Breakpoint):
    def stop(self):
        global started
        # CreateEvent ABI verified in this guarded binary: this=rdi,input=rsi.
        data = bytes(gdb.selected_inferior().read_memory(int(gdb.parse_and_eval('$rsi')),24))
        kind,flags,code,value = struct.unpack_from('<HHIi',data,12)
        if kind != 1 or code not in (0xe0,0xe4,0x17):
            return False
        now = time.monotonic()
        if started is None:
            started = now
        print('KEY_DELIVERY code=%d value=%d diagnostic_host_ms=%.3f' %
              (code,value,(now-started)*1000),flush=True)
        return False

class TabEntry(gdb.Breakpoint):
    def stop(self):
        global tab_started,browser_pid
        if tab_started is not None:
            return False
        tab_started = time.monotonic()
        browser_pid = int(current()['process'].dereference()['pid'])
        print('NEW_TAB_ENTER diagnostic_host_ms=%.3f' %
              ((tab_started-started)*1000 if started is not None else -1),flush=True)
        return False

class PresentEntry(gdb.Breakpoint):
    def stop(self):
        if tab_started is None or int(gdb.parse_and_eval('(process_t*)$rdi').dereference()['pid']) != browser_pid:
            return False
        print('FIRST_PRESENT_ENTER after_tab_ms=%.3f' %
              ((time.monotonic()-tab_started)*1000),flush=True)
        gdb.post_event(lambda: gdb.execute('detach'))
        gdb.post_event(lambda: gdb.execute('quit'))
        return True

KeyEntry('*0x45515040',gdb.BP_HARDWARE_BREAKPOINT,internal=True)
TabEntry('*0x51e88fb0',gdb.BP_HARDWARE_BREAKPOINT,internal=True)
PresentEntry('window_server_present',gdb.BP_HARDWARE_BREAKPOINT,internal=True)
end
continue
