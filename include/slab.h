#ifndef LITEOS_SLAB_H
#define LITEOS_SLAB_H

#include "buddy.h"

#define LITEOS_SLAB_MAX_SLABS 64U
#define LITEOS_SLAB_MAX_OBJECTS \
    (LITEOS_BUDDY_MIN_BLOCK_SIZE / sizeof(VOID *))
#define LITEOS_SLAB_BITMAP_BYTES \
    ((LITEOS_SLAB_MAX_OBJECTS + 7U) / 8U)

typedef struct {
    LITEOS_PHYSICAL_BLOCK PhysicalBlock;
    VOID *FreeObjects;
    UINT32 ObjectCount;
    UINT32 FreeCount;
    /* 每一位表示对应槽位当前是否已经分配，防止重复释放破坏空闲链。 */
    UINT8 AllocationBits[LITEOS_SLAB_BITMAP_BYTES];
} LITEOS_SLAB;

typedef struct {
    UINT64 ObjectSize;
    UINT64 ObjectStride;
    UINT64 ObjectAlignment;
    UINT32 SlabCount;
    LITEOS_SLAB Slabs[LITEOS_SLAB_MAX_SLABS];
} LITEOS_SLAB_CACHE;

/* 初始化一个固定大小的内核对象缓存。 */
BOOLEAN liteos_slab_cache_init(LITEOS_SLAB_CACHE *cache, UINT64 object_size,
                               UINT64 object_alignment);

/* 从缓存中分配和释放对象。 */
VOID *liteos_slab_alloc(LITEOS_SLAB_CACHE *cache);
BOOLEAN liteos_slab_free(LITEOS_SLAB_CACHE *cache, VOID *object);

/* 释放缓存拥有的全部 Buddy slab。 */
VOID liteos_slab_cache_destroy(LITEOS_SLAB_CACHE *cache);

#endif
