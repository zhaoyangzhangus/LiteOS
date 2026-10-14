"""Production address probe must observe during mouse hold and clean up errors."""
import ast
from pathlib import Path
from types import SimpleNamespace

root = Path(__file__).resolve().parents[2]
source = (root / 'tools/chromium/qmp-address-latency.py').read_text()
tree = ast.parse(source)
helper = next(n for n in tree.body if isinstance(n, ast.ClassDef) and n.name == 'MouseHold')
for failure in (None, 'press', 'capture'):
    calls = []
    clock = [0.0]
    def sleep(seconds):
        clock[0] += seconds
    def command(name, args):
        assert name == 'input-send-event'
        down = args['events'][0]['data']['down']
        calls.append(down)
        if down and failure == 'press':
            raise RuntimeError('partial down delivery')
    env = {'time': SimpleNamespace(monotonic=lambda: clock[0], sleep=sleep),
           'command': command}
    exec(compile(ast.Module(body=[helper], type_ignores=[]), '<production-mouse-hold>', 'exec'), env)
    try:
        with env['MouseHold']() as mouse:
            mouse.press()
            assert clock[0] == 0 and calls == [True]  # no blocking hold
            mouse.release()
            assert calls == [True]
            if failure == 'capture':
                raise RuntimeError('screenshot failed')
            clock[0] = .025
            mouse.release()
            mouse.release()
    except RuntimeError:
        assert failure is not None
    else:
        assert failure is None
    assert calls == [True, False] and not mouse.down
    assert clock[0] >= .02
assert 'time.sleep(.005)' in source
assert "'visibility_interval_ms'" in source and "'frame_capture_ms'" in source
print('Address probe: nonblocking hold, deadline release, partial delivery/capture cleanup passed')
