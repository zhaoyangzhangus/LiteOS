#include <kernel/kmem.h>
#include <kernel/perf.h>
#include <kernel/telemetry.h>

bool kernel_perf_benchmark(kernel_perf_report_t *report) {
    uint64_t minimum = UINT64_MAX;
    uint64_t maximum = 0U;
    uint64_t total = 0U;
    uint64_t hits_before;
    uint64_t hits_after;
    uint64_t refills_before;
    uint64_t refills_after;

    if (report == 0) return false;
    /* 预热后每次只做一个固定大小的小对象分配，保持 magazine 热路径稳定。 */
    for (uint32_t i = 0; i < 32U; ++i) {
        void *memory = kmalloc(64U, 0);
        if (memory == 0) return false;
        kfree(memory);
    }
    hits_before = kmem_fastpath_hits();
    refills_before = kmem_fastpath_refills();
    for (uint32_t i = 0; i < KERNEL_PERF_SAMPLE_COUNT; ++i) {
        uint64_t start = telemetry_timestamp();
        void *memory = kmalloc(64U, 0);
        uint64_t elapsed;
        if (memory == 0) return false;
        elapsed = telemetry_timestamp() - start;
        kfree(memory);
        if (elapsed < minimum) minimum = elapsed;
        if (elapsed > maximum) maximum = elapsed;
        total += elapsed;
    }
    hits_after = kmem_fastpath_hits();
    refills_after = kmem_fastpath_refills();
    report->samples = KERNEL_PERF_SAMPLE_COUNT;
    report->min_tsc = minimum;
    report->average_tsc = total / KERNEL_PERF_SAMPLE_COUNT;
    report->max_tsc = maximum;
    report->fastpath_hits = hits_after - hits_before;
    report->fastpath_refills = refills_after - refills_before;
    return report->fastpath_hits != 0U && report->min_tsc <= report->max_tsc;
}
