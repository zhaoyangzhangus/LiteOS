#pragma once

#include <stdint.h>
#include <stdbool.h>

#include <kernel/block_device.h>

/* USB Mass Storage: Bulk-Only Transport + SCSI transparent subclass. */
/* LITEOS_USB_ROOT_PATCH_V1: boot-stage synchronous MSC attach. */
bool usb_msc_attach(uint8_t slot);
bool usb_msc_schedule_attach(uint8_t slot);
void usb_msc_detach(uint8_t slot);
bool usb_msc_present(uint8_t slot);
LITEOS_BLOCK_DEVICE *usb_msc_block_device(uint8_t slot);
