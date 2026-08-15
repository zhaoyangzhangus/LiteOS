#pragma once
#include "abi.h"

enum os_vm_prot {
    OS_VM_READ  = 1u << 0,
    OS_VM_WRITE = 1u << 1,
    OS_VM_EXEC  = 1u << 2,
};

enum os_vm_flags {
    OS_VM_PRIVATE = 1u << 0,
    OS_VM_SHARED  = 1u << 1,
    OS_VM_FIXED   = 1u << 2,
    OS_VM_STACK   = 1u << 3,
};

typedef struct os_vm_map_args {
    os_versioned_header_t hdr;
    uint64_t address;
    uint64_t length;
    uint64_t offset;
    os_handle_t object;
    uint32_t prot;
    uint32_t flags;
} os_vm_map_args_t;
