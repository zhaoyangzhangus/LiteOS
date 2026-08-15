#ifndef LITEOS_KERNEL_TELEMETRY_H
#define LITEOS_KERNEL_TELEMETRY_H

#include "base.h"

#define TELEMETRY_RING_CAPACITY 256U

/* 统一的内核快路径分类，用户态只读取记录，不依赖实现细节。 */
#define TELEMETRY_CATEGORY_IO_LATENCY        1U
#define TELEMETRY_CATEGORY_STORAGE_BATCH     2U
#define TELEMETRY_CATEGORY_NETWORK_BATCH     3U
#define TELEMETRY_CATEGORY_GPU_SUBMIT        4U
#define TELEMETRY_CATEGORY_POWER_TRANSACTION 5U
#define TELEMETRY_CATEGORY_GPU_BATCH         6U

typedef struct telemetry_record {
    uint64_t sequence;
    uint64_t timestamp_tsc;
    uint32_t cpu;
    uint32_t category;
    uint64_t object_id;
    uint64_t value0;
    uint64_t value1;
} telemetry_record_t;

bool telemetry_init(void);
uint64_t telemetry_timestamp(void);
kstatus_t telemetry_record(uint32_t category, uint64_t object_id,
                           uint64_t value0, uint64_t value1);
kstatus_t telemetry_record_latency(uint32_t category, uint64_t object_id,
                                   uint64_t start_tsc);
size_t telemetry_snapshot(uint64_t after_sequence, telemetry_record_t *records,
                          size_t capacity, uint64_t *next_sequence);
bool telemetry_self_test(void);

#endif
