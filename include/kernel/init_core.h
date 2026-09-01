#pragma once

#include <kernel/bootinfo.h>

typedef struct liteos_init_core_hooks {
    void (*write)(const CHAR8 *text);
    void (*halt)(void);
} liteos_init_core_hooks_t;

/* Run the allocator/core memory gates before storage and scheduling. */
BOOLEAN liteos_init_core(const liteos_init_core_hooks_t *hooks);
