"""A successful GDB exit is not sufficient to accept a profile snapshot."""
import ast
import json
from pathlib import Path
from types import SimpleNamespace

root = Path(__file__).resolve().parents[2]
tree = ast.parse((root/'tools/chromium/guest-delay-profile.py').read_text())
block = next(n for n in reversed(tree.body) if isinstance(n, ast.Try))

class Target:
    def __init__(self, writes): self.writes=writes
    def __truediv__(self, name): return self
    def write_text(self, value): self.writes.append(json.loads(value))

for output, valid in [
    ('PROFILE {"action":"read","selected_process":125}\n', True),
    ('Python Exception: missing record\n', False),
    ('PROFILE {"action":"begin"}\n', False),
    ('PROFILE {"action":"read"}\nPROFILE {"action":"read"}\n', False),
    ('PROFILE not-json\n', False),
]:
    calls=[]
    writes=[]
    def command(name, arguments=None):
        calls.append(name)
        return {'running':True} if name=='query-status' else None
    env={'subprocess':SimpleNamespace(run=lambda *a,**k:SimpleNamespace(
             stdout=output,stderr='')),
         'root':Target(writes),'code':'unused','args':SimpleNamespace(action='read'),
         'json':json,'command':command,'print':lambda *a,**k:None}
    try:
        exec(compile(ast.Module(body=[block],type_ignores=[]),
                     '<production-profile-result>','exec'),env)
    except (RuntimeError, json.JSONDecodeError):
        assert not valid
    else:
        assert valid
    assert calls==['human-monitor-command','cont','query-status']
    assert bool(writes)==valid
print('Profile result identity, missing/duplicate/malformed rejection, persisted evidence and unconditional resume passed')
