#pragma once

#include <kernel/bootinfo.h>

typedef struct liteos_init_userspace_hooks {
    void (*write)(const CHAR8 *text);
    void (*write_u32)(UINT32 value);
    void (*halt)(void);
} liteos_init_userspace_hooks_t;

/* Publish the initial user services, IPC, window server, and runtime workers. */
BOOLEAN liteos_init_userspace(const liteos_init_userspace_hooks_t *hooks);

/* Complete the single-CPU compositor handoff after kernel_main changes stack. */
BOOLEAN liteos_userspace_start_window_worker(
    const liteos_init_userspace_hooks_t *hooks);

/* Run the user ELF/process/scheduler runtime gate after display setup. */
void liteos_userspace_run_runtime_self_test(
    const liteos_init_userspace_hooks_t *hooks);
