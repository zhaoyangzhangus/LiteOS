#pragma once

#include <kernel/bootinfo.h>

typedef struct liteos_init_network_hooks {
    void (*write)(const CHAR8 *text);
    void (*write_u32)(UINT32 value);
    void (*halt)(void);
} liteos_init_network_hooks_t;

/* Validate the network stack and publish the live network boundary. */
BOOLEAN liteos_init_network(const liteos_init_network_hooks_t *hooks);
