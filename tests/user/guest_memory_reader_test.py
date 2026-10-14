"""Production diagnostic walker: explicit roots, page splits and huge leaves."""
from pathlib import Path
import sys

root=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(root/'tools/chromium'))
from guest_memory import read_user,DIRECT_MAP

class Memory:
    def __init__(self):
        self.data={}
    def store(self,address,data):
        self.data.update({address+i:value for i,value in enumerate(data)})
    def pte(self,address,value):
        self.store(address,value.to_bytes(8,'little'))
    def read_memory(self,address,size):
        assert address>=DIRECT_MAP
        return bytes(self.data[address-DIRECT_MAP+i] for i in range(size))

memory=Memory()
memory.pte(0x1000,0x2001)
memory.pte(0x2000,0x3001)
memory.pte(0x3000,0x4001)
memory.pte(0x4000+16*8,0x5001)
memory.pte(0x4000+17*8,0x6001)
memory.store(0x5ffe,b'ab')
memory.store(0x6000,b'cd')
assert read_user(memory,0x1007,0x10ffe,4)==b'abcd'
memory.pte(0x3000,0x200081)
memory.store(0x210000,b'huge2m')
assert read_user(memory,0x1000,0x10000,6)==b'huge2m'
memory.pte(0x2000,0x40000081)
memory.store(0x40010000,b'huge1g')
assert read_user(memory,0x1000,0x10000,6)==b'huge1g'
memory.pte(0x2000,0)
try:
    read_user(memory,0x1000,0x10000,1)
except ValueError:
    pass
else:
    raise AssertionError('unmapped page accepted')
for address,size in [(0,1),(0x10000,-1),(0x10000,16*1024*1024+1),
                     (0x7fffffffffff,2)]:
    try:
        read_user(memory,0x1000,address,size)
    except ValueError:
        pass
    else:
        raise AssertionError('invalid range accepted')
print('Guest memory reader: explicit CR3, PCID masking, cross-page reads, '
      '2MiB/1GiB leaves, missing pages and bounded ranges passed')
