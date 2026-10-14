#pragma once

#include "abi.h"

/* Physical memory statistics in base pages.  free_pages includes pages held
 * in the allocator's per-CPU caches, since those remain immediately usable. */
typedef struct os_memory_info {
    os_versioned_header_t hdr;
    uint64_t page_size;
    uint64_t total_pages;
    uint64_t free_pages;
} os_memory_info_t;
