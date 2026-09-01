#pragma once

#include <kernel/bootinfo.h>

typedef struct liteos_init_memory_hooks {
    void (*write)(const CHAR8 *text);
    void (*halt)(void);
    BOOLEAN (*console_init)(const LITEOS_BOOT_INFO *info,
                            UINT64 framebuffer_virtual_base);
    void (*console_disable)(void);
} liteos_init_memory_hooks_t;

/* Bring up the canonical memory map and publish the memory boundary. */
BOOLEAN liteos_init_memory(LITEOS_BOOT_INFO *info,
                           const liteos_init_memory_hooks_t *hooks,
                           UINT64 *framebuffer_virtual_base);

/* Reuse the same WC framebuffer mapping after a high-half hand-off. */
BOOLEAN liteos_map_framebuffer_wc(const LITEOS_BOOT_INFO *info,
                                  UINT64 *virtual_base);
