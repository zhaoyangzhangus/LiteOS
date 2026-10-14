#pragma once

#include <kernel/bootinfo.h>

/*
 * Early console setup is kept separate from kernel_entry().  Text output is
 * exposed by console.h; this header contains only backend lifecycle hooks and
 * therefore cannot become a second serial-output API.
 */
BOOLEAN kernel_console_init(const OS_BOOT_INFO *info,
                            UINT64 framebuffer_virtual_base);
BOOLEAN kernel_console_init_early(const OS_BOOT_INFO *info);
void kernel_console_disable(void);
void kernel_console_serial_init(void);
