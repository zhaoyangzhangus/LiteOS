#pragma once

#include <kernel/bootinfo.h>
#include <kernel/device.h>
#include <kernel/nvme_core.h>

BOOLEAN filesystem_mount_usb_root(const LITEOS_BOOT_INFO *boot_info);
BOOLEAN filesystem_mount_nvme_root(const LITEOS_BOOT_INFO *boot_info,
                                   device_t *fallback_device);
BOOLEAN filesystem_mount_all_volumes(const LITEOS_BOOT_INFO *boot_info,
                                     const nvme_controller_t *active_controller,
                                     BOOLEAN *root_is_nvme);
