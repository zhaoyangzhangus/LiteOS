"""Exercise production COW fast-path branches with host page-table fixtures."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/mm/fault.c').read_text()

def branch(marker):
    start = source.index(marker)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

stale = branch('if (x86_page_entry_writable(existing_flags))')
stale = stale.replace('__asm__ volatile ("invlpg (%0)" : :\n'
                      '                              "r"(page_address) : "memory");',
                      '++invalidations;')
assert '__asm__' not in stale
upgrade = branch('if (page_to_phys(page).value == existing.value)')
assert source.index(upgrade) < source.index('status = x86_unmap_page(',
                                          source.index(upgrade))
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
typedef int kstatus_t;
typedef uint64_t vaddr_t;
typedef struct { uint64_t value; } paddr_t;
typedef struct { paddr_t root_table; atomic_uint_fast64_t tlb_generation; } vm_space_t;
typedef struct { uint32_t prot; } vm_area_t;
typedef struct { uint64_t pte_ready_tsc; } vm_fault_trace_t;
typedef struct { paddr_t physical; } page_t;
#define K_OK 0
#define K_EIO -5
#define X86_CACHE_WB 0
static unsigned invalidations, unlocked, protects;
static kstatus_t protect_result;
static bool x86_page_entry_writable(uint64_t flags) { return flags & 2; }
static paddr_t page_to_phys(page_t *page) { return page->physical; }
static void map_unlock(vm_space_t *space) { (void)space; ++unlocked; }
static uint64_t telemetry_timestamp(void) { return 123; }
static uint32_t hardware_flags(uint32_t prot, bool cow) { assert(!cow); return prot; }
static kstatus_t x86_protect_page(paddr_t root, vaddr_t va, uint32_t flags, int cache) {
    assert(root.value == 4096 && va == 8192 && flags == 7 && cache == X86_CACHE_WB);
    ++protects; return protect_result;
}
static kstatus_t stale_fault(vm_space_t *space, uint64_t existing_flags) {
    const uint64_t page_address = 8192;
    (void)page_address;
''' + stale + r'''
    return 42;
}
static kstatus_t cow_fault(vm_space_t *space, page_t *page, paddr_t existing,
                          vm_fault_trace_t *trace) {
    const uint64_t page_address = 8192;
    vm_area_t area_value = {7}, *area = &area_value;
    kstatus_t status;
''' + upgrade + r'''
    return 42; /* Different physical page must use the original remap path. */
}
int main(void) {
    vm_space_t space = {{4096}, 0};
    page_t page = {{16384}};
    vm_fault_trace_t trace = {0};
    for (unsigned i=0; i<1000; ++i) {
        assert(stale_fault(&space, 2) == K_OK);
        assert(stale_fault(&space, 0) == 42);
    }
    assert(invalidations == 1000 && unlocked == 1000);
    protect_result = K_OK;
    assert(cow_fault(&space, &page, page.physical, &trace) == K_OK);
    assert(protects == 1 && unlocked == 1001 && space.tlb_generation == 1);
    assert(trace.pte_ready_tsc == 123);
    assert(cow_fault(&space, &page, (paddr_t){32768}, &trace) == 42);
    assert(protects == 1 && unlocked == 1001 && space.tlb_generation == 1);
    protect_result = K_EIO;
    assert(cow_fault(&space, &page, page.physical, 0) == K_EIO);
    assert(protects == 2 && unlocked == 1002 && space.tlb_generation == 1);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-cow-upgrade-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('COW fast paths: stale local TLB retries, same-page upgrade, different-page fallback and failure bookkeeping passed')
