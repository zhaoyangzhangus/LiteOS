set pagination off
set confirm off
python
import gdb, struct, json, sys
from pathlib import Path
sys.path.insert(0, str(Path('tools/chromium').resolve()))
from trace_build import validate_chrome_build
expected = 'Build ID: 35be57647f4096ab'
try:
    validate_chrome_build(expected)
except RuntimeError as error:
    raise gdb.GdbError('Refusing stale Chrome diagnostic: ' + str(error))
end
target remote /tmp/system-keys.sock
python
count=0
class Trace(gdb.Breakpoint):
    def __init__(self,address,label):
        super().__init__(address,gdb.BP_HARDWARE_BREAKPOINT,internal=True)
        self.label=label
    def stop(self):
        global count
        cpu=gdb.selected_thread().num-1
        if not 0<=cpu<int(gdb.parse_and_eval('g_cpu_count')):return False
        thread=gdb.parse_and_eval('g_cpus[%d].queue.current'%cpu).dereference()
        if not int(thread['process']):return False
        process=thread['process'].dereference()
        if process['name'].string().rsplit('/',1)[-1]!='chrome' or count>=80:return False
        count+=1;p=int(gdb.parse_and_eval('$rdi'))
        record={'stage':self.label,'pid':int(process['pid']),'object':hex(p)}
        if self.label=='STATE':record['state']=int(gdb.parse_and_eval('$rsi'))
        if self.label in ('LOCK','UNLOCK'):
            record['count']=struct.unpack('<I',bytes(gdb.selected_inferior().read_memory(p+60,4)))[0]
        if self.label=='CURSOR':
            manager=struct.unpack('<Q',bytes(gdb.selected_inferior().read_memory(p+8,8)))[0]
            if manager:record['xy']=struct.unpack('<ii',bytes(gdb.selected_inferior().read_memory(manager+32,8)))
        print('MENU_BUTTON '+json.dumps(record),flush=True)
        return False
Trace('*0x4e8d0390','UNLOCK')
Trace('*0x4e8d0170','LOCK')
Trace('*0x4e8bdc20','STATE')
Trace('*0x45513ee0','CURSOR')
end
continue
