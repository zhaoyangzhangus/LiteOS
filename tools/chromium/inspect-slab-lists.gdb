set pagination off
target remote /tmp/system-slab.sock
set $caches = &g_caches
add-symbol-file build/kmalloc-inspection-types.o 0
python
import gdb
base=int(gdb.parse_and_eval('$caches'))
kind=gdb.lookup_type('kmem_cache_t')
for index in range(10):
    cache=gdb.Value(base+index*kind.sizeof).cast(kind.pointer()).dereference()
    slab=cache['slabs']
    visited=set()
    free_slots=0
    while int(slab):
        address=int(slab)
        if address in visited or len(visited)>=100000:
            raise RuntimeError('Cycle or oversized slab list')
        visited.add(address)
        free_slots+=int(slab.dereference()['free_count'])
        slab=slab.dereference()['next']
    print('class=%d slot_size=%d partial_slabs=%d free_slots=%d' % (index,int(cache['slot_size']),len(visited),free_slots))
end
detach
quit
