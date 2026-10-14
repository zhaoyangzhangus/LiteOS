"""Run production draw trace callbacks without connecting to a debugger."""
from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import time
from types import SimpleNamespace

ROOT=Path(__file__).resolve().parents[2]
script=(ROOT/'tools/chromium/trace-draw-paint-pair.gdb').read_text()
body=script.split('target remote ',1)[1].split('python\n',1)[1].split('\nend',1)[0]

class Reference:
    def dereference(self): return {'pid':1}

for mode in ('return','decision','invalid'):
    records=[]
    class Breakpoint:
        def __init__(self,address,*args,**kwargs): records.append(address)
    values={'$rsp':0x10000,'*(unsigned long long*)$rsp':0x40110000,'$rdi':0x20000}
    debugger=SimpleNamespace(Breakpoint=Breakpoint,BP_HARDWARE_BREAKPOINT=1,
                             GdbError=RuntimeError,parse_and_eval=lambda name:values[name])
    environment=SimpleNamespace(environ={'OS_DRAW_TRACE':mode})
    namespace={'gdb':debugger,'os':environment,'json':json,'time':time}
    with redirect_stdout(io.StringIO()):
        if mode=='invalid':
            try: exec(compile(body,str(ROOT/'tools/chromium/trace-draw-paint-pair.gdb'),'exec'),namespace)
            except RuntimeError: pass
            else: raise AssertionError('invalid trace mode accepted')
            assert not records
            continue
        exec(compile(body,str(ROOT/'tools/chromium/trace-draw-paint-pair.gdb'),'exec'),namespace)
        assert len(records)==(3 if mode=='return' else 4)
        namespace.update(started=time.monotonic(),browser=1,
                         current=lambda:{'process':Reference(),'tid':2})
        draw=namespace['Draw'].__new__(namespace['Draw'])
        for _ in range(100):
            assert draw.stop() is False
            assert len(records)<=4
        assert namespace['calls']==16 and len(records)==4
        assert len(set(records))==4
        if mode=='return':
            assert records[-1]=='*0x40110000'
            assert namespace['return_pc']==0x40110000
        else:
            assert namespace['return_pc'] is None
print('Draw trace: actual callback execution, both mutually exclusive modes, 100 invocations, 16-call cap and four live breakpoint bound passed')
