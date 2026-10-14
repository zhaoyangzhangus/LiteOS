#pragma once

#include <kernel/bootinfo.h>

/* Seed the kernel random stream from the UEFI RNG before userspace starts. */
void random_init(const OS_BOOT_INFO *boot_info);
