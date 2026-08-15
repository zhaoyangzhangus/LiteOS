#pragma once

#include "mm.h"
#include "../bootinfo.h"

/* 在最终页表建立后初始化稀疏 PFN 数据库和经典 Buddy。 */
bool liteos_mm_init(LITEOS_BOOT_INFO *boot_info);

/*
 * 最终直接映射的 RAM 白名单。prepare 在低端引导映射仍可用时复制固件范围，
 * enable 只能在对应的稀疏页表已经切换完成后调用。
 */
bool direct_map_prepare_ram_ranges(const LITEOS_BOOT_INFO *boot_info);
bool direct_map_range_is_ram(paddr_t start, size_t size);
void direct_map_enable_validation(void);
