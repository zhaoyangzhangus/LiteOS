"""Address-latency shortcuts must release modifiers before mouse input."""
import ast
from pathlib import Path
from types import SimpleNamespace

root = Path(__file__).resolve().parents[2]
def load_helper(path, name):
    tree = ast.parse((root / path).read_text())
    return next(n for n in tree.body if isinstance(n, ast.FunctionDef)
                and n.name == name)

helper = load_helper('tools/chromium/qmp-address-latency.py', 'key_chord')

for codes in [('ctrl', 'r'), ('ctrl', 'l'), ('f6',)]:
    for failure in [None, 'down', 'sleep']:
        calls = []
        sleeps = []

        def command(name, arguments):
            assert name == 'input-send-event'
            events = arguments['events']
            calls.append([(e['data']['key']['data'], e['data']['down'])
                          for e in events])
            if failure == 'down' and events[0]['data']['down']:
                raise RuntimeError('partial key delivery')

        def sleep(seconds):
            sleeps.append(seconds)
            if failure == 'sleep':
                raise RuntimeError('interrupted hold')

        env = {'command': command, 'time': SimpleNamespace(sleep=sleep)}
        exec(compile(ast.Module(body=[helper], type_ignores=[]),
                     '<production-address-chord>', 'exec'), env)
        try:
            env['key_chord'](*codes, hold_ms=40)
        except RuntimeError:
            assert failure is not None
        else:
            assert failure is None
        assert calls == [[(code, True) for code in codes],
                         [(code, False) for code in reversed(codes)]]
        assert sleeps == ([] if failure == 'down' else [.04])

print('Address shortcuts: simultaneous key down, reverse release, failure cleanup passed')

helper = load_helper('tools/chromium/qmp-open-url.py', 'key')
calls = []
sleeps = []
env = {'command': lambda name, arguments: calls.append((name, arguments)),
       'time': SimpleNamespace(sleep=sleeps.append)}
exec(compile(ast.Module(body=[helper], type_ignores=[]),
             '<production-url-key>', 'exec'), env)
for codes in [('ctrl', 'l'), ('shift', 'semicolon'), ('a',)]:
    env['key'](*codes, hold=100)
    down, up = calls[-2:]
    assert down[0] == up[0] == 'input-send-event'
    assert [(e['data']['key']['data'], e['data']['down'])
            for e in down[1]['events']] == [(c, True) for c in codes]
    assert [(e['data']['key']['data'], e['data']['down'])
            for e in up[1]['events']] == [(c, False) for c in reversed(codes)]
    assert sleeps[-2:] == [.1, .05]
print('URL typing: Ctrl/Shift released before following character passed')

tree = ast.parse((root / 'tools/chromium/qmp-open-url.py').read_text())
typed_loop = next(i for i,n in enumerate(tree.body)
                  if isinstance(n,ast.For) and ast.unparse(n.target)=='codes')
assert ast.unparse(tree.body[typed_loop+1]) == "key('delete')"
assert ast.unparse(tree.body[typed_loop+2]).startswith("command('screendump'")
assert ast.unparse(tree.body[typed_loop+3]) == "key('ret')"
env['key']('delete')
assert [events['events'][0]['data']['key']['data'] for _,events in calls[-2:]] == ['delete','delete']
assert [events['events'][0]['data']['down'] for _,events in calls[-2:]] == [True,False]
print('URL navigation: selected history suffix deletion precedes evidence screenshot and Enter passed')
