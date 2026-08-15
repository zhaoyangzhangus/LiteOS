#pragma once
#include "abi.h"

typedef struct os_thread_create {
    os_versioned_header_t hdr;
    uint64_t entry;
    uint64_t stack_top;
    uint64_t fs_base;
    uint64_t argument;
    uint32_t flags;
    uint32_t reserved;
} os_thread_create_t;
