#pragma once

#include <kernel/bootinfo.h>

typedef struct liteos_init_scheduler_hooks {
    void (*write)(const CHAR8 *text);
    void (*write_u32)(UINT32 value);
    void (*halt)(void);
} liteos_init_scheduler_hooks_t;

/* Start the APs, publish the scheduler, and run the early kernel gates. */
BOOLEAN liteos_init_scheduler(const LITEOS_BOOT_INFO *boot_info,
                              const liteos_init_scheduler_hooks_t *hooks);

/* Transitional self-test for the legacy queue API; it is not the live scheduler. */
