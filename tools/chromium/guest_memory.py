"""Read stopped guest userspace through an explicit x86-64 page-table root."""
DIRECT_MAP = 0xffff800000000000
PAGE_MASK = 0x000ffffffffff000


def read_user(memory, root, address, size):
    if not (0x10000 <= address < 0x800000000000 and
            0 <= size <= 16*1024*1024 and address+size <= 0x800000000000):
        raise ValueError('Invalid diagnostic userspace range')
    result = bytearray()
    while size:
        table = root & PAGE_MASK
        for shift in (39,30,21,12):
            data = memory.read_memory(DIRECT_MAP+table+((address>>shift)&511)*8,8)
            pte = int.from_bytes(bytes(data),'little')
            if not pte & 1:
                raise ValueError('Unmapped captured user page')
            if shift in (30,21) and pte & 128:
                physical=(pte&PAGE_MASK&~((1<<shift)-1))+(address&((1<<shift)-1))
                break
            table=pte&PAGE_MASK
        else:
            physical=table+(address&4095)
        length=min(size,4096-(address&4095))
        result.extend(memory.read_memory(DIRECT_MAP+physical,length))
        address+=length
        size-=length
    return bytes(result)
