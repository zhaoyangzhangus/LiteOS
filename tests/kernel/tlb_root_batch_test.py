"""Test actual root flush/dispatch/target selection with mocked privileged ops."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/arch/x86_64/mm/tlb.c').read_text()
def function(marker):
    start = source.index(marker)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

flush = function('static void invalidate_target_root(')
def mock_instruction(match):
    text = match[0]
    if 'invpcid' in text:
        return 'assert(descriptor.pcid == pcid && descriptor.address == 0); ++inactive_flushes;'
    if '"=r"(cr3)' in text:
        return 'cr3 = mock_cr3;'
    assert 'mov %0, %%cr3' in text
    return 'mock_cr3 = cr3; ++root_flushes;'
flush = re.sub(r'__asm__ volatile \(.*?\);', mock_instruction, flush, flags=re.S)
dispatch = function('void x86_tlb_ipi_interrupt(')
shootdown = function('static bool tlb_shootdown(')
shootdown = shootdown.replace('__asm__ volatile ("pause");', '(void)0;')
assert '__asm__' not in flush + shootdown
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#define MAX_CPUS 4
#define X86_64_USER_TOP ((1ULL<<47)-1)
typedef uint64_t vaddr_t;
typedef struct { uint64_t value; } paddr_t;
typedef struct { uint64_t pcid, address; } invpcid_descriptor_t;
typedef struct { uint32_t apic_id; } cpu_t;
typedef struct { unsigned cpu_count; cpu_t cpus[MAX_CPUS]; } x86_acpi_platform_t;
static x86_acpi_platform_t platform = {4, {{0},{1},{2},{3}}};
static struct { bool pcid, invpcid; } x86_boot_cpu_features;
static uint64_t cpu_roots[4] = {4096,4096,8192,8192};
static unsigned current_cpu, root_flushes, inactive_flushes, page_flushes;
static unsigned sends, failures, lock_depth, completions;
static int fail_send=-1, drop_ack=-1;
static uint64_t clock_ticks, mock_cr3 = (1ULL<<63)|4096|7;
static atomic_uint_fast64_t g_tlb_generation, g_tlb_acknowledgement[MAX_CPUS];
static paddr_t g_tlb_root;
static vaddr_t g_tlb_address;
static uint16_t g_tlb_pcid;
static bool g_tlb_full_root;
static paddr_t x86_current_root_table(void) { return (paddr_t){cpu_roots[current_cpu]}; }
static bool x86_is_canonical(uint64_t address) {
    return address <= X86_64_USER_TOP || address >= 0xffff800000000000ULL;
}
static void invalidate_target_page(paddr_t root, uint16_t pcid, vaddr_t va) {
    (void)root;(void)pcid;(void)va;++page_flushes;
}
static uint32_t x86_current_cpu_index(void) { return current_cpu; }
static void lapic_end_of_interrupt(void) { ++completions; }
static void shootdown_lock(void) { assert(!lock_depth); ++lock_depth; }
static uint64_t tlb_enable_ipi_delivery(uint32_t cpu) { (void)cpu; return 0x202; }
static uint16_t root_pcid(paddr_t root) { assert(root.value); return 7; }
static const x86_acpi_platform_t *x86_acpi_platform(void) { return &platform; }
static bool x86_smp_cpu_online(uint32_t cpu) { return cpu < 4; }
static bool sched_cpu_uses_root(uint32_t cpu, paddr_t root) { return cpu_roots[cpu]==root.value; }
static void tlb_finish_shootdown(uint32_t cpu, uint64_t flags) {
    assert(cpu==0 && flags==0x202 && lock_depth==1); --lock_depth;
}
static uint64_t x86_read_tsc(void) { return clock_ticks+=100; }
static uint64_t x86_timeout_ns_to_tsc(uint64_t ns) { (void)ns; return 1000; }
static void tlb_failure_report(const char *reason, uint32_t owner, uint32_t target,
 uint32_t apic, uint64_t generation, uint64_t ack, paddr_t root, vaddr_t address) {
    (void)reason;(void)owner;(void)target;(void)apic;(void)generation;
    (void)ack;(void)root;(void)address;++failures;
}
''' + flush + '\n' + dispatch + r'''
static bool send_tlb_ipi_with_retry(uint32_t apic) {
    ++sends;
    if ((int)apic==fail_send) return false;
    if ((int)apic!=drop_ack) {
        unsigned saved=current_cpu;current_cpu=apic;
        x86_tlb_ipi_interrupt();current_cpu=saved;
    }
    return true;
}
''' + shootdown + '\n' + function('bool x86_tlb_shootdown_page(') + '\n' + function('bool x86_tlb_shootdown_root(') + r'''
static void reset(void) {
    root_flushes=inactive_flushes=page_flushes=sends=failures=completions=0;
    fail_send=drop_ack=-1;current_cpu=0;clock_ticks=0;
}
int main(void) {
    reset();assert(x86_tlb_shootdown_root((paddr_t){4096}));
    assert(root_flushes==2 && sends==1 && completions==1 && page_flushes==0 && !lock_depth);
    assert(!(mock_cr3 & (1ULL<<63)));
    reset();assert(x86_tlb_shootdown_page((paddr_t){4096},8192));
    assert(page_flushes==2 && sends==1 && root_flushes==0 && !lock_depth);
    reset();assert(x86_tlb_shootdown_page((paddr_t){4096},0xffff800000005000ULL));
    assert(page_flushes==4 && sends==3 && !lock_depth);
    reset();fail_send=1;assert(!x86_tlb_shootdown_root((paddr_t){4096}));
    assert(failures==1 && sends==1 && !lock_depth);
    reset();drop_ack=1;assert(!x86_tlb_shootdown_root((paddr_t){4096}));
    assert(failures==1 && sends==1 && !lock_depth);
    reset();assert(!x86_tlb_shootdown_root((paddr_t){0}));
    assert(!sends && !lock_depth);
    x86_boot_cpu_features.pcid=x86_boot_cpu_features.invpcid=true;
    reset();invalidate_target_root((paddr_t){8192},9);
    assert(inactive_flushes==1 && root_flushes==0);
    x86_boot_cpu_features.invpcid=false;
    invalidate_target_root((paddr_t){8192},9);
    assert(inactive_flushes==1 && root_flushes==0);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-tlb-root-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('TLB root batching: local/remote flush, inactive PCID, target selection, page/global dispatch and failure cleanup passed')
