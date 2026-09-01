#pragma once
#ifndef LITEOS_KERNEL_PERF_H
#define LITEOS_KERNEL_PERF_H

#include "base.h"

#define KERNEL_PERF_SAMPLE_COUNT 128U

typedef struct kernel_perf_report {
    uint64_t samples;
    uint64_t min_tsc;
    uint64_t median_tsc;
    uint64_t p95_tsc;
    uint64_t average_tsc;
    uint64_t max_tsc;
    uint64_t fastpath_hits;
    uint64_t fastpath_refills;
} kernel_perf_report_t;

bool kernel_perf_benchmark(kernel_perf_report_t *report);
/* Location-aware boundary used by the macro below.  Keeping the file and
 * line at the call site makes a metric as actionable as a debug stage. */
void kernel_perf_emit_scope_at(const char *name, uint64_t start_tsc,
                              const char *file, uint32_t line);
void kernel_perf_emit_scope(const char *name, uint64_t start_tsc);
void kernel_perf_emit_value_at(const char *name, uint64_t value,
                              const char *file, uint32_t line);
void kernel_perf_emit_value(const char *name, uint64_t value);
/* Emit the fixed kmalloc report from its perf Owner while retaining the
 * caller's source location for debugger and benchmark diagnostics. */
void kernel_perf_emit_report_at(const kernel_perf_report_t *report,
                               const char *file, uint32_t line);
void kernel_perf_emit_report(const kernel_perf_report_t *report);

#define kernel_perf_emit_scope(name, start_tsc) \
    kernel_perf_emit_scope_at((name), (start_tsc), __FILE__, __LINE__)

#define kernel_perf_emit_value(name, value) \
    kernel_perf_emit_value_at((name), (value), __FILE__, __LINE__)

#define kernel_perf_emit_report(report) \
    kernel_perf_emit_report_at((report), __FILE__, __LINE__)

#endif
