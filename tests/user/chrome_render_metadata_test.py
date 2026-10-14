#!/usr/bin/env python3
"""Run the diagnostic's real reader against synthetic process page tables."""
import ast
from pathlib import Path
import re
import struct
import types

root = Path(__file__).resolve().parents[2]
script = (root / 'tools/chromium/trace-root-render-pass.gdb').read_text()
code = re.findall(r'^python\n(.*?)^end$', script, re.M | re.S)[1]
parsed = ast.parse(code)
function = next(n for n in parsed.body if isinstance(n, ast.FunctionDef) and n.name == 'read_user')
direct = 0xffff800000000000
storage = {}
class Pointer:
    def __init__(self, value): self.value = value
    def dereference(self): return self.value
class ReadError(Exception):
    pass
class Memory:
    def __init__(self): self.reads = []
    def read_memory(self, address, size):
        assert address >= direct, 'Reader used the debugger-selected user CR3'
        self.reads.append((address, size))
        return bytes(storage.get(address+i, 0) for i in range(size))
memory = Memory()
def store(address, data):
    storage.update({direct+address+i: value for i, value in enumerate(data)})
def entry(table, index, value): store(table+index*8, struct.pack('<Q', value))
process = {'vm': Pointer({'root_table': {'value': 0x1000}})}
scope = {'struct': struct, 'memory': memory,
         'current': lambda: {'process': Pointer(process)},
         'gdb': types.SimpleNamespace(GdbError=ReadError)}
exec(compile(ast.Module(body=[function], type_ignores=[]), '<actual metadata reader>', 'exec'), scope)
read = scope['read_user']
entry(0x1000, 0, 0x2005)
entry(0x2000, 0, 0x3005)
entry(0x3000, 0, 0x4005)
entry(0x4000, 4, 0x9005)
entry(0x4000, 5, 0xa005)
store(0x9ffe, b'AB'); store(0xa000, b'CD')
assert read(0x4ffe, 4) == b'ABCD'
entry(0x3000, 1, 0x200085)
store(0x202000, b'huge')
assert read(0x202000, 4) == b'huge'
entry(0x2000, 1, 0x40000085)
store(0x40002000, b'page')
assert read(0x40002000, 4) == b'page'
def rejected(address, size):
    try: read(address, size)
    except ReadError: return
    raise AssertionError('Invalid/nonresident read was accepted')
entry(0x4000, 4, 0x9001)
rejected(0x4000, 4)
entry(0x4000, 4, 0)
rejected(0x4000, 4)
for address, size in ((0, 4), (direct, 4), (0x7ffffffffffc, 8), (0x4000, 0), (0x4000, 65)):
    before = len(memory.reads)
    rejected(address, size)
    assert len(memory.reads) == before
reader = ast.unparse(function)
assert 'write_memory' not in reader and 'gdb.execute' not in reader
assert 'entry&5!=5' in code and 'size<=64' in code
print('Actual render metadata reader: owner VM, cross-page, 2MiB/1GiB pages, bounds, user permissions and nonresident rejection passed')
