#!/usr/bin/env python3
"""Execute the production page_alloc dispatcher with mocked allocator zones."""
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
source = (root / "kernel/mm/phys/percpu_page.c").read_text()
start = source.index("page_t *page_alloc(uint8_t order, page_alloc_flags_t flags) {")
end = source.index("\nvoid page_free(", start)
dispatcher = source[start:end]
mock = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
typedef struct { int zone; } page_t;
typedef unsigned page_alloc_flags_t;
#define PAGE_LIKELY(x) (x)
#define PAGE_UNLIKELY(x) (x)
#define MAX_CPUS 256U
#define PAGE_ZONE_DMA32 0U
static page_t normal_page, dma_page, slow_page;
static page_t *normal_result, *dma_result;
static unsigned cpu, normal_calls, dma_calls, slow_calls, stats, disabled;
static uint64_t page_pool_irq_save(void) { return 42; }
static void page_pool_irq_restore(uint64_t f) { assert(f == 42 && !disabled); }
static void x86_preempt_disable_fast(void) { ++disabled; }
static void x86_preempt_enable_fast(void) { assert(disabled == 1); --disabled; }
static unsigned x86_current_cpu_index_fast(void) { return cpu; }
static page_t *pool_alloc_o0_normal(unsigned c) {
    assert(disabled && c == cpu); ++normal_calls;
    return c < MAX_CPUS ? normal_result : 0;
}
static page_t *pool_alloc_small(unsigned z, unsigned o, unsigned c) {
    assert(disabled && z == PAGE_ZONE_DMA32 && o == 0 && c < MAX_CPUS);
    ++dma_calls; return dma_result;
}
static void page_stats_note_alloc(unsigned o) { assert(o == 0); ++stats; }
static page_t *page_alloc_slow(unsigned o, unsigned f) {
    assert(o || f); ++slow_calls; return &slow_page;
}
'''
cases = r'''
int main(void) {
    normal_result = &normal_page; dma_result = &dma_page;
    assert(page_alloc(0, 0) == &normal_page && dma_calls == 0 && stats == 1);
    normal_result = 0;
    assert(page_alloc(0, 0) == &dma_page && dma_calls == 1 && stats == 2);
    dma_result = 0;
    assert(page_alloc(0, 0) == 0 && dma_calls == 2 && stats == 2);
    cpu = MAX_CPUS;
    assert(page_alloc(0, 0) == 0 && dma_calls == 2 && stats == 2);
    assert(page_alloc(1, 0) == &slow_page);
    assert(page_alloc(0, 1) == &slow_page);
    assert(slow_calls == 2 && normal_calls == 4 && !disabled);
    puts("page-alloc-fast: normal, DMA32 fallback, exhaustion, CPU bounds, slow dispatch OK");
}
'''
with tempfile.TemporaryDirectory(prefix="system-page-alloc-test-") as directory:
    binary = str(pathlib.Path(directory) / "test")
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-x", "c", "-", "-o", binary],
                   input=mock + dispatcher + cases, text=True, check=True)
    subprocess.run([binary], check=True)
