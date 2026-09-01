#pragma once

#include <kernel/bootinfo.h>
#include "nvme_core.h"

typedef struct liteos_init_storage_hooks {
    void (*write)(const CHAR8 *text);
    void (*write_u32)(UINT32 value);
    void (*halt)(void);
} liteos_init_storage_hooks_t;

/* Validate the storage controller path and return the active controller. */
BOOLEAN liteos_init_storage(const liteos_init_storage_hooks_t *hooks,
                            const nvme_controller_t **active_controller);
