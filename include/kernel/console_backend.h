#pragma once

#include <kernel/bootinfo.h>

/*
 * Early console setup is kept separate from kernel_entry().  Text output is
 * exposed by console.h; this header contains only backend lifecycle hooks and
 * therefore cannot become a second serial-output API.
 */
BOOLEAN liteos_console_init(const LITEOS_BOOT_INFO *info,
                            UINT64 framebuffer_virtual_base);
BOOLEAN liteos_console_init_early(const LITEOS_BOOT_INFO *info);
void liteos_console_disable(void);
void liteos_console_serial_init(void);
