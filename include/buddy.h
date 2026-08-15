#ifndef LITEOS_BUDDY_H
#define LITEOS_BUDDY_H

#include "bootinfo.h"

/* order 0 对应一个 4 KiB 物理页。 */
#define LITEOS_BUDDY_MIN_ORDER       0U
#define LITEOS_BUDDY_MAX_ORDER       31U
#define LITEOS_BUDDY_MIN_BLOCK_SIZE  (4ULL * 1024ULL)

typedef struct {
    UINT64 PhysicalAddress;
    UINT32 Order;
    UINT32 Reserved;
} LITEOS_PHYSICAL_BLOCK;

BOOLEAN liteos_buddy_init(const LITEOS_BOOT_INFO *boot_info);
/* 迁移期兼容层：让旧模块统一从规范 page_t Buddy 获取物理页。 */
BOOLEAN liteos_buddy_bind_canonical_allocator(VOID);
BOOLEAN liteos_buddy_alloc(UINT32 order, LITEOS_PHYSICAL_BLOCK *block);
BOOLEAN liteos_buddy_alloc_bytes(UINT64 bytes, LITEOS_PHYSICAL_BLOCK *block);
BOOLEAN liteos_buddy_free(LITEOS_PHYSICAL_BLOCK *block);

UINT64 liteos_buddy_total_bytes(void);
UINT64 liteos_buddy_free_bytes(void);
UINT64 liteos_buddy_free_block_count(UINT32 order);

#endif
