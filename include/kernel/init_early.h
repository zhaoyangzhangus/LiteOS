#pragma once

#include <kernel/bootinfo.h>

typedef struct kernel_init_early_hooks {
    void (*write)(const CHAR8 *text);
    void (*halt)(void);
} kernel_init_early_hooks_t;

/* Validate loader state and bring up the CPU/early platform boundary. */
BOOLEAN kernel_init_early(OS_BOOT_INFO *info,
                          const kernel_init_early_hooks_t *hooks);
