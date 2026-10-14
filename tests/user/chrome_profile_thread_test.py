#!/usr/bin/env python3
"""Execute the production GDB thread selector without touching a guest."""
import ast
import json
from pathlib import Path
import types

root = Path(__file__).resolve().parents[2]
source = (root / 'tools/chromium/guest-delay-profile.py').read_text()
tree = ast.parse(source)
assignment = next(n for n in tree.body if isinstance(n, ast.Assign)
                  and any(isinstance(t, ast.Name) and t.id == 'code' for t in n.targets))
value = assignment.value
while isinstance(value, ast.Call):
    value = value.func.value
code = value.value
function = next(n for n in ast.parse(code).body if isinstance(n, ast.FunctionDef)
                and n.name == 'select_profile_thread')
process, head, offset = 1000, 1032, 16
objects = {
    (head, 'list_head_t'): {'next': 2016},
    (2000, 'thread_t'): {'tid': 474, 'process': process, 'process_node': {'next': 3016}},
    (3000, 'thread_t'): {'tid': 497, 'process': process, 'process_node': {'next': head}},
}
class SelectionError(Exception):
    pass
def evaluate(expression):
    return 32 if '->threads' in expression else offset
scope = {'gdb': types.SimpleNamespace(parse_and_eval=evaluate, GdbError=SelectionError),
         'cast': lambda address, kind: objects[address, kind]}
exec(compile(ast.Module(body=[function], type_ignores=[]), '<actual GDB selector>', 'exec'), scope)
select = scope['select_profile_thread']
assert select(process, 0, 474) == 474
assert select(process, 474, 474) == 474
assert select(process, 497, 474) == 497
def rejected(tid):
    try:
        select(process, tid, 474)
    except SelectionError:
        return
    raise AssertionError('Invalid thread was selected')
rejected(999)
objects[3000, 'thread_t']['process'] = 2000
rejected(497)
objects[3000, 'thread_t']['process'] = process
objects[3000, 'thread_t']['process_node']['next'] = 2016
rejected(999)  # Corrupt/transient cycle is bounded, not an infinite traversal.
objects[head, 'list_head_t']['next'] = head
rejected(497)  # Exited thread cannot be selected from an empty list.
assert "'selected_tid':tid" in source
assert 'select_profile_thread(process_address,requested_tid,leader)' in source
parsed = ast.parse(code)
selectors = ast.literal_eval(next(n.value for n in parsed.body if isinstance(n, ast.Assign)
                                 and any(isinstance(t, ast.Name) and t.id == 'selectors' for t in n.targets)))
dispatch = next(n for n in parsed.body if isinstance(n, ast.If)
                and isinstance(n.test, ast.Compare)
                and isinstance(n.test.left, ast.Name) and n.test.left.id == 'action'
                and isinstance(n.test.ops[0], ast.Eq)
                and n.test.comparators[0].value == 'begin')
writes = []
exec(compile(ast.Module(body=dispatch.orelse[0].body, type_ignores=[]),
             '<actual profile shutdown>', 'exec'),
     {'selectors': selectors, 'write': lambda name, value: writes.append((name, value)),
      'raw': lambda name, count: [dict(writes)[name]],
      'json': json, 'print': lambda *args: None, 'action': 'end'})
assert writes[:-2] == [(selector, 0) for selector in selectors]
assert writes[-2:] == [('g_sched_wake_trace_threshold_ns', 10000000),
                      ('g_vm_lock_trace_threshold_ns', 10000000)]
print('Actual profile selector: main/default, browser Viz, foreign/stale/exited threads and bounded cycle rejection passed')
print('Actual profile shutdown: all selectors disabled before restoring the historical 10ms threshold passed')
