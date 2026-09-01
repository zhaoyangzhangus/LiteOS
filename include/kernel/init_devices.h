#pragma once

#include <kernel/bootinfo.h>

typedef struct liteos_init_devices_hooks {
    void (*write)(const CHAR8 *text);
    void (*write_u32)(UINT32 value);
    void (*halt)(void);
} liteos_init_devices_hooks_t;

/* Discover platform devices and complete the deterministic device self-tests. */
BOOLEAN liteos_init_devices(LITEOS_BOOT_INFO *info,
                            const liteos_init_devices_hooks_t *hooks);
