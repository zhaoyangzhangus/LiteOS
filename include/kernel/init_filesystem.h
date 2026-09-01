#pragma once

#include <kernel/bootinfo.h>
#include "nvme_core.h"

typedef struct liteos_init_filesystem_hooks {
    void (*write)(const CHAR8 *text);
    void (*write_u32)(UINT32 value);
    void (*halt)(void);
} liteos_init_filesystem_hooks_t;

/* Mount the boot root and validate the VFS, file mapping, journal, and LiteFS paths. */
BOOLEAN liteos_prepare_realtest_root(const LITEOS_BOOT_INFO *boot_info);
BOOLEAN liteos_init_filesystem(const LITEOS_BOOT_INFO *boot_info,
                               const nvme_controller_t *active_controller,
                               const liteos_init_filesystem_hooks_t *hooks);
