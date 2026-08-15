#ifndef LITEOS_PAGING_H
#define LITEOS_PAGING_H

#include "uefi.h"
#include "bootinfo.h"

/* 建立高半内核、直接映射和受控早期恒等映射。 */
BOOLEAN liteos_enable_kernel_paging(const LITEOS_BOOT_INFO *boot_info);
/* 物理页分配器上线后，用固件 RAM 范围替换临时的宽松直接映射。 */
BOOLEAN liteos_rebuild_ram_direct_map(const LITEOS_BOOT_INFO *boot_info);
UINT64 liteos_identity_pml4_address(void);
/* 调用者必须已在高半地址执行，并且当前栈也不能依赖低端恒等映射。 */
BOOLEAN liteos_drop_identity_mapping(void);

#endif
