#pragma once

#include <kernel/bootinfo.h>

typedef struct kernel_init_core_hooks {
    void (*write)(const CHAR8 *text);
    void (*halt)(void);
} kernel_init_core_hooks_t;

/* Run the allocator/core memory gates before storage and scheduling. */
BOOLEAN kernel_init_core(const kernel_init_core_hooks_t *hooks);
