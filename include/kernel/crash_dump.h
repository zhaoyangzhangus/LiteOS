#pragma once

#include <arch/x86_64/interrupt.h>
#include <kernel/base.h>

#define CRASH_DUMP_MAGIC 0x4C444D50U /* "LDMP" */
#define CRASH_DUMP_VERSION 1U
#define CRASH_DUMP_RECORD_COUNT 8U

typedef struct crash_dump_record {
    volatile uint32_t sequence;
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t vector;
    uint32_t cpu_index;
    uint64_t error_code;
    uint64_t rip;
    uint64_t rsp;
    uint64_t rflags;
    uint64_t cr2;
} crash_dump_record_t;

void crash_dump_capture(const arch_trap_frame_t *frame, uint64_t cr2);
bool crash_dump_read_latest(crash_dump_record_t *out);
uint32_t crash_dump_count(void);
bool crash_dump_self_test(void);
