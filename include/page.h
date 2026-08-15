#ifndef LITEOS_PAGE_H
#define LITEOS_PAGE_H

#include "bootinfo.h"

/* Page Descriptor 的基础状态位。 */
enum {
    LITEOS_PAGE_PRESENT  = 1U << 0,
    LITEOS_PAGE_RESERVED = 1U << 1,
    LITEOS_PAGE_METADATA = 1U << 2,
};

typedef struct {
    UINT64 PhysicalAddress;
    UINT32 Flags;
    UINT32 ReferenceCount;
    UINT64 Mapping;
    UINT64 Private;
} LITEOS_PAGE;

/* 根据物理地址查找 Page Descriptor。 */
LITEOS_PAGE *liteos_page_lookup(UINT64 physical_address);

/* 建立物理页描述表；调用前必须已经初始化 Buddy 和恒等映射。 */
BOOLEAN liteos_page_init(const LITEOS_BOOT_INFO *boot_info);

#endif
