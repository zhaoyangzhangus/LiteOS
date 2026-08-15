#pragma once
#include "abi.h"

typedef uint64_t os_gpu_va_t;

typedef struct os_gpu_create_context {
    os_versioned_header_t hdr;
    os_handle_t device;
    uint64_t flags;
} os_gpu_create_context_t;

typedef struct os_gpu_submit {
    os_versioned_header_t hdr;
    os_handle_t context;
    os_handle_t command_buffer;
    os_handle_t signal_fence;
    uint64_t signal_value;
    uint64_t command_offset;
    uint64_t command_length;
} os_gpu_submit_t;
