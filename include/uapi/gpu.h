#pragma once
#pragma once
#include "abi.h"

typedef uint64_t os_gpu_va_t;

typedef struct os_gpu_create_context {
    os_versioned_header_t hdr;
    os_handle_t device;
    uint64_t flags;
} os_gpu_create_context_t;

typedef struct os_gpu_alloc {
    os_versioned_header_t hdr;
    uint64_t size;
    uint32_t flags;
    uint32_t reserved;
} os_gpu_alloc_t;

enum os_gpu_map_flags {
    OS_GPU_MAP_FIXED = 1u << 0,
};

typedef struct os_gpu_map {
    os_versioned_header_t hdr;
    os_handle_t allocation;
    uint64_t address;
    uint64_t offset;
    uint64_t length;
    uint32_t prot;
    uint32_t flags;
} os_gpu_map_t;

typedef struct os_gpu_submit {
    os_versioned_header_t hdr;
    os_handle_t context;
    os_handle_t command_buffer;
    os_handle_t signal_fence;
    uint64_t signal_value;
    uint64_t command_offset;
    uint64_t command_length;
} os_gpu_submit_t;

/* GPU_SUBMIT 在 signal_fence 为无效句柄时创建 fence 并原位写回句柄。 */
