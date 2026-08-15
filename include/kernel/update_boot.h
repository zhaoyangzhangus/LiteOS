#ifndef LITEOS_KERNEL_UPDATE_BOOT_H
#define LITEOS_KERNEL_UPDATE_BOOT_H

#include <stdbool.h>

#include "bootinfo.h"

/* 在内核仍使用 Loader 建立的物理运行时映射时提交 pending 槽。 */
bool kernel_update_commit_boot(const LITEOS_BOOT_INFO *info);

#endif
