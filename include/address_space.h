#ifndef LITEOS_ADDRESS_SPACE_H
#define LITEOS_ADDRESS_SPACE_H

#include "buddy.h"

#define LITEOS_USER_SPACE_BASE (1ULL << 40)
#define LITEOS_USER_SPACE_SIZE (2ULL * 1024ULL * 1024ULL)
#define LITEOS_USER_SPACE_ORDER 9U
#define LITEOS_ADDRESS_SPACE_TABLE_ORDER 2U

typedef struct {
    LITEOS_PHYSICAL_BLOCK PageTableBlock;
    LITEOS_PHYSICAL_BLOCK UserMemoryBlock;
    UINT64 Pml4Physical;
    UINT64 UserVirtualBase;
    UINT64 UserVirtualSize;
    BOOLEAN Initialized;
    BOOLEAN Active;
} LITEOS_ADDRESS_SPACE;

BOOLEAN liteos_address_space_create(LITEOS_ADDRESS_SPACE *space);
BOOLEAN liteos_address_space_activate(LITEOS_ADDRESS_SPACE *space);
BOOLEAN liteos_address_space_handle_page_fault(UINT64 fault_address,
                                               UINT64 error_code);
BOOLEAN liteos_page_fault_handle(UINT64 error_code, UINT64 fault_address);
BOOLEAN liteos_address_space_destroy(LITEOS_ADDRESS_SPACE *space);

#endif
