#pragma once

#include <kernel/bootinfo.h>

typedef struct liteos_init_early_hooks {
    void (*write)(const CHAR8 *text);
    void (*halt)(void);
} liteos_init_early_hooks_t;

/* Validate loader state and bring up the CPU/early platform boundary. */
BOOLEAN liteos_init_early(LITEOS_BOOT_INFO *info,
                          const liteos_init_early_hooks_t *hooks);
