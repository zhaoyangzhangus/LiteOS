#pragma once
#include <stdint.h>
#include <stddef.h>

typedef uint64_t os_handle_t;
typedef int64_t os_status_t;

#define OS_INVALID_HANDLE ((os_handle_t)0)

typedef struct os_versioned_header {
    uint32_t size;
    uint16_t version;
    uint16_t flags;
} os_versioned_header_t;

typedef struct os_timespec {
    int64_t seconds;
    int32_t nanoseconds;
    int32_t reserved;
} os_timespec_t;
