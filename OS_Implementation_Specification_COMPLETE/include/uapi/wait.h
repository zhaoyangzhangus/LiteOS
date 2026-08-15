#pragma once
#include "abi.h"

#define OS_WAIT_ALL            (1U << 0)
#define OS_WAIT_INDEX_ALL      UINT32_MAX
#define OS_WAIT_MAX_HANDLES    1024U
#define OS_WAIT_INFINITE       UINT64_MAX

typedef struct os_wait_result {
    uint32_t index;
    uint32_t reserved;
    int64_t value;
} os_wait_result_t;

typedef struct os_wait_many {
    os_versioned_header_t hdr;
    uint32_t count;
    uint32_t wait_flags;
    uint64_t handles;       /* 指向 os_handle_t[count] 的用户地址。 */
    uint64_t timeout_ns;
    uint32_t result_index;
    uint32_t reserved;
    int64_t result_value;
} os_wait_many_t;
