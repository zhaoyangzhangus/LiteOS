"""Read actual browser Skia CPU flags/dispatch pointers in the sole QEMU."""
import json
from pathlib import Path
import subprocess
import re
import os

root = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(root / 'tools'))
from qmp_client import QMPClient
sys.path.insert(0, str(root / 'tools/chromium'))
from trace_build import chrome_binary_path, validate_chrome_build
source = os.environ.get('CHROMIUM_SRC')
if not source:
    raise RuntimeError('Set CHROMIUM_SRC for the Skia symbol toolchain')
chromium = Path(source)
expected = 'Build ID: 5c734622f8e4d5fe'
validate_chrome_build(expected)
binary = chrome_binary_path()
names = {
    'features':'SkCpu::gCachedFeatures',
    'mask_pointer':'SkOpts::blit_mask_d32_a8',
    'filter_pointer':'SkOpts::S32_alpha_D32_filter_DX',
    'mask_ssse3':'ssse3::blit_mask_d32_a8(unsigned int*, unsigned long, unsigned char const*, unsigned long, unsigned int, int, int)',
    'filter_ssse3':'ssse3::S32_alpha_D32_filter_DX(SkBitmapProcState const&, unsigned int const*, int, unsigned int*)',
}
symbols = {}
bias = int(re.search(r'#define ELF_PIE_BIAS\s+(0x[0-9a-fA-F]+)',
                     (root/'kernel/process/exec_internal.h').read_text()).group(1),16)
process = subprocess.Popen([str(chromium/'third_party/llvm-build/Release+Asserts/bin/llvm-nm'),
                            '-C','--defined-only',str(binary)],
                           stdout=subprocess.PIPE,text=True)
for line in process.stdout:
    fields = line.rstrip('\n').split(' ',2)
    if len(fields)==3 and fields[2] in names.values():
        symbols[fields[2]]=int(fields[0],16)+bias
if process.wait()!=0 or len(symbols)!=len(names):
    raise RuntimeError('Missing current Skia symbols')
subprocess.run(['cc','-g3','-fno-eliminate-unused-debug-types','-ffreestanding',
                '-DOS_KERNEL_BUILD','-Iinclude','-c','tools/chromium/inspection-types.c',
                '-o','build/current-inspection-types.o'],cwd=root,check=True)
code = 'locations='+repr({key:symbols[value] for key,value in names.items()})+'\n'+r'''
import gdb,json,struct
memory=gdb.selected_inferior()
def cast(p,name):return gdb.Value(p).cast(gdb.lookup_type(name).pointer()).dereference()
registry=int(gdb.parse_and_eval('&g_process_registry'))
offset=int(gdb.parse_and_eval('(unsigned long)&((process_t*)0)->global_node'))
node=int(cast(registry,'list_head_t')['next']);seen=set();browsers=[]
while node!=registry and node not in seen and len(seen)<256:
    seen.add(node);p=cast(node-offset,'process_t')
    if p['name'].string().rsplit('/',1)[-1]=='chrome' and int(p['thread_count'])>1:
        browsers.append((int(p['pid']),int(p['vm'])))
    node=int(p['global_node']['next'])
if not browsers:raise gdb.GdbError('No current browser')
pid,vm=min(browsers);table=int(cast(vm,'vm_space_t')['root_table']['value'])
def u64(address):return struct.unpack('<Q',bytes(memory.read_memory(address,8)))[0]
def read_user(va,size):
    physical=table;mask=0x000ffffffffff000
    for shift in (39,30,21,12):
        entry=u64(0xffff800000000000+physical+((va>>shift)&511)*8)
        if not entry&1:raise gdb.GdbError('Unmapped Skia symbol')
        if shift in (30,21) and entry&128:
            physical=(entry&mask)&~((1<<shift)-1)
            physical+=va&((1<<shift)-1)
            break
        physical=entry&mask
    else:physical+=va&4095
    if (va&4095)+size>4096:raise gdb.GdbError('Unexpected symbol alignment')
    return int.from_bytes(bytes(memory.read_memory(0xffff800000000000+physical,size)),'little')
features=read_user(locations['features'],4)
dispatch={}
for kind in ('mask','filter'):
    pointer=read_user(locations[kind+'_pointer'],8)
    dispatch[kind]={'pointer':hex(pointer),'ssse3_selected':pointer==locations[kind+'_ssse3']}
print('SKIA_CPU '+json.dumps({'pid':pid,'features':features,'dispatch':dispatch}))
'''
qmp = QMPClient()
def command(name,args=None):
    return qmp.command(name, args)

assert command('query-status')['running'], 'Do not change an externally stopped guest'
try:
    command('human-monitor-command',{'command-line':'gdbserver unix:/tmp/system-skia.sock,server=on,wait=off'})
    result=subprocess.run(['gdb','-q','-nx','-batch',str(root/'build/qemu-running-kernel.elf'),
        '-ex','add-symbol-file build/current-inspection-types.o 0',
        '-ex','target remote /tmp/system-skia.sock',
        '-ex','python exec('+repr(code)+')','-ex','detach'],cwd=root,
        check=True,timeout=15,capture_output=True,text=True)
    print(result.stdout,end='')
    matches=[line for line in result.stdout.splitlines() if line.startswith('SKIA_CPU ')]
    if len(matches)!=1 or any(marker in result.stdout+result.stderr for marker in
            ('Python Exception','Error occurred in Python','Traceback')):
        raise RuntimeError('Invalid Skia snapshot: '+result.stderr)
    data=json.loads(matches[0][len('SKIA_CPU '):])
    (root/'build/chrome-skia-cpu.json').write_text(json.dumps(data,indent=2)+'\n')
finally:
    command('human-monitor-command',{'command-line':'gdbserver none'})
    command('cont')
    assert command('query-status')['running']
