#ifndef LITEOS_KERNEL_PERF_H
#define LITEOS_KERNEL_PERF_H

#include "base.h"

#define KERNEL_PERF_SAMPLE_COUNT 128U

typedef struct kernel_perf_report {
    uint64_t samples;
    uint64_t min_tsc;
    uint64_t average_tsc;
    uint64_t max_tsc;
    uint64_t fastpath_hits;
    uint64_t fastpath_refills;
} kernel_perf_report_t;

bool kernel_perf_benchmark(kernel_perf_report_t *report);

#endif
