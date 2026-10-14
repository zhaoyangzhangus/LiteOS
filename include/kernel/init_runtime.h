#pragma once

#include <kernel/bootinfo.h>

/* Common boot failure owner shared by ordered init units and the runtime handoff. */
__attribute__((noreturn)) void kernel_halt_forever(void);
__attribute__((noreturn)) void kernel_halt_forever_at(
    const char *file, uint32_t line);

/* Complete the high-half handoff and run the post-init Ring0 continuation. */
__attribute__((noreturn)) void kernel_init_runtime_start(
    OS_BOOT_INFO *info, UINT64 framebuffer_virtual_base);
