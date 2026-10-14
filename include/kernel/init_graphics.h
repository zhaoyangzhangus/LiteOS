#pragma once

#include <kernel/bootinfo.h>

typedef struct kernel_init_graphics_hooks {
    void (*write)(const CHAR8 *text);
    void (*write_u32)(UINT32 value);
    void (*halt)(void);
} kernel_init_graphics_hooks_t;

/* Run the graphics/input/syscall boot self-tests at one explicit boundary. */
BOOLEAN kernel_init_graphics(const OS_BOOT_INFO *info,
                             const kernel_init_graphics_hooks_t *hooks);
