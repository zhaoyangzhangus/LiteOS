#pragma once

#include <kernel/bootinfo.h>
#include <kernel/nvme_core.h>
#include <stdbool.h>

bool kernel_init_canonical_device_dma_io_self_test(void);
bool kernel_init_pci_self_test(void);
bool kernel_init_nvme_self_test(void);

/* Run the post-scheduler gates before the high-half runtime handoff. */
BOOLEAN kernel_init_post_scheduler(const OS_BOOT_INFO *info,
                                   const nvme_controller_t *active_controller);
