#pragma once

#include "abi.h"

/* Stable clock identifiers shared by the kernel and freestanding libc. */
#define OS_CLOCK_REALTIME  0U
#define OS_CLOCK_MONOTONIC 1U

typedef struct os_clock_set {
    os_versioned_header_t hdr;
    uint32_t clock_id;
    uint32_t reserved;
    os_timespec_t value;
} os_clock_set_t;
