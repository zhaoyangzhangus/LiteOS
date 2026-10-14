"""Direct key chord probe uses concurrent hold, with exception-safe release."""
from pathlib import Path
import ast
from types import SimpleNamespace

root = Path(__file__).resolve().parents[2]
tree = ast.parse((root/'tools/chromium/qmp-tab-close-test.py').read_text())
selected = [n for n in tree.body if isinstance(n,(ast.FunctionDef,ast.ClassDef))
            and n.name in ('release_chord','ChordGuard')]
assert len(selected)==2
class Clock:
    now=10.0
    sleeps=[]
    def monotonic(self):return self.now
    def sleep(self,n):
        assert 0<=n<=.201
        self.sleeps.append(n)
        self.now+=n
clock = Clock()
calls=[]
env={'time':clock,'command':lambda n,a:calls.append((n,a)),
     'keys_down':True,'key_release_due':10.2}
exec(compile(ast.Module(body=selected,type_ignores=[]),'<production-chord>','exec'),env)
env['release_chord']()
assert not calls and env['keys_down']
env['release_chord'](wait=True)
assert len(calls)==1 and not env['keys_down'] and clock.sleeps
events=calls[0][1]['events']
assert calls[0][0]=='input-send-event'
assert [(e['data']['key']['data'],e['data']['down']) for e in events]==[('t',False),('ctrl',False)]
env['release_chord'](wait=True)
assert len(calls)==1
env['keys_down']=True
try:
    with env['ChordGuard']():
        raise RuntimeError('frame capture failed')
except RuntimeError:
    pass
assert len(calls)==2 and not env['keys_down']
chord = next(n for n in ast.walk(tree) if isinstance(n,ast.If)
             and isinstance(n.test,ast.Compare)
             and ast.unparse(n.test)=="args.key_injection == 'chord'")
env['args']=SimpleNamespace(key_injection='chord',key_hold_ms=200)
exec(compile(ast.Module(body=[chord],type_ignores=[]),'<production-key-down>','exec'),env)
assert env['keys_down'] and calls[-1][0]=='input-send-event'
assert [(e['data']['key']['data'],e['data']['down']) for e in calls[-1][1]['events']]==[('ctrl',True),('t',True)]
assert env['key_release_due']==clock.now+.2
env['release_chord'](wait=True)
def partial_delivery(n,a):
    if a['events'][0]['data']['down']:
        raise RuntimeError('delivery interrupted after modifier press')
    calls.append((n,a))
env['command']=partial_delivery
try:
    with env['ChordGuard']():
        exec(compile(ast.Module(body=[chord],type_ignores=[]),'<partial-key-down>','exec'),env)
except RuntimeError:
    pass
assert not env['keys_down'] and calls[-1][1]['events'][0]['data']['down'] is False
print('Direct Ctrl+T chord, concurrent hold, reverse release and exception cleanup passed')
