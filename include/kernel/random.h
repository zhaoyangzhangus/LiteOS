#pragma once

#include <kernel/bootinfo.h>

/* Seed the kernel random stream from the UEFI RNG before userspace starts. */
void liteos_random_init(const LITEOS_BOOT_INFO *boot_info);
