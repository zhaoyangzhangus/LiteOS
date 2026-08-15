#include "slab.h"

static UINT64 block_size(UINT32 order) {
    if (order > LITEOS_BUDDY_MAX_ORDER) return 0;
    return LITEOS_BUDDY_MIN_BLOCK_SIZE << order;
}

static BOOLEAN is_power_of_two(UINT64 value) {
    return value != 0 && (value & (value - 1ULL)) == 0;
}

static UINT64 align_up(UINT64 value, UINT64 alignment) {
    return (value + alignment - 1ULL) & ~(alignment - 1ULL);
}

static BOOLEAN slab_object_index(const LITEOS_SLAB_CACHE *cache,
                                 const LITEOS_SLAB *slab, UINT64 address,
                                 UINT32 *index) {
    UINT64 start;
    UINT64 size;
    UINT64 offset;
    UINT64 object_bytes;
    if (cache == 0 || slab == 0 || index == 0 || cache->ObjectStride == 0) return 0;
    start = slab->PhysicalBlock.PhysicalAddress;
    size = block_size(slab->PhysicalBlock.Order);
    if (size == 0 || address < start || address - start >= size) return 0;
    offset = address - start;
    if (offset % cache->ObjectStride != 0) return 0;
    object_bytes = (UINT64)slab->ObjectCount * cache->ObjectStride;
    if (object_bytes == 0 || offset >= object_bytes) return 0;
    *index = (UINT32)(offset / cache->ObjectStride);
    return *index < slab->ObjectCount && *index < LITEOS_SLAB_MAX_OBJECTS;
}

static BOOLEAN slab_object_allocated(const LITEOS_SLAB *slab, UINT32 index) {
    if (slab == 0 || index >= LITEOS_SLAB_MAX_OBJECTS) return 0;
    return (slab->AllocationBits[index >> 3U] &
            (UINT8)(1U << (index & 7U))) != 0U;
}

static VOID slab_set_object_allocated(LITEOS_SLAB *slab, UINT32 index,
                                       BOOLEAN allocated) {
    UINT8 mask;
    if (slab == 0 || index >= LITEOS_SLAB_MAX_OBJECTS) return;
    mask = (UINT8)(1U << (index & 7U));
    if (allocated) slab->AllocationBits[index >> 3U] |= mask;
    else slab->AllocationBits[index >> 3U] &= (UINT8)~mask;
}

static BOOLEAN add_slab(LITEOS_SLAB_CACHE *cache) {
    if (cache->SlabCount >= LITEOS_SLAB_MAX_SLABS) return 0;
    LITEOS_SLAB *slab = &cache->Slabs[cache->SlabCount];
    if (!liteos_buddy_alloc(LITEOS_BUDDY_MIN_ORDER, &slab->PhysicalBlock)) return 0;

    UINT64 slab_bytes = block_size(slab->PhysicalBlock.Order);
    UINT64 object_count = slab_bytes / cache->ObjectStride;
    if (object_count == 0 || object_count > (UINT32)-1 ||
        object_count > LITEOS_SLAB_MAX_OBJECTS) {
        liteos_buddy_free(&slab->PhysicalBlock);
        return 0;
    }

    UINT8 *base = (UINT8 *)(uintptr_t)slab->PhysicalBlock.PhysicalAddress;
    for (UINT64 i = 0; i < object_count; ++i) {
        VOID **object = (VOID **)(base + i * cache->ObjectStride);
        *object = i + 1ULL < object_count ?
                  (VOID *)(base + (i + 1ULL) * cache->ObjectStride) : 0;
    }
    slab->FreeObjects = base;
    slab->ObjectCount = (UINT32)object_count;
    slab->FreeCount = (UINT32)object_count;
    for (UINT32 i = 0; i < LITEOS_SLAB_BITMAP_BYTES; ++i) {
        slab->AllocationBits[i] = 0U;
    }
    ++cache->SlabCount;
    return 1;
}

BOOLEAN liteos_slab_cache_init(LITEOS_SLAB_CACHE *cache, UINT64 object_size,
                               UINT64 object_alignment) {
    if (cache == 0 || object_size == 0 || !is_power_of_two(object_alignment) ||
        object_alignment > LITEOS_BUDDY_MIN_BLOCK_SIZE ||
        object_size > (UINT64)-1 - sizeof(VOID *) ||
        object_size + object_alignment - 1ULL > (UINT64)-1) return 0;
    cache->ObjectSize = object_size;
    cache->ObjectAlignment = object_alignment;
    cache->ObjectStride = align_up(object_size < sizeof(VOID *) ? sizeof(VOID *) : object_size,
                                   object_alignment);
    cache->SlabCount = 0;
    for (UINT32 i = 0; i < LITEOS_SLAB_MAX_SLABS; ++i) {
        cache->Slabs[i].PhysicalBlock.PhysicalAddress = 0;
        cache->Slabs[i].PhysicalBlock.Order = 0;
        cache->Slabs[i].PhysicalBlock.Reserved = 0;
        cache->Slabs[i].FreeObjects = 0;
        cache->Slabs[i].ObjectCount = 0;
        cache->Slabs[i].FreeCount = 0;
    }
    return 1;
}

VOID *liteos_slab_alloc(LITEOS_SLAB_CACHE *cache) {
    if (cache == 0 || cache->ObjectStride == 0) return 0;
    for (UINT32 i = 0; i < cache->SlabCount; ++i) {
        LITEOS_SLAB *slab = &cache->Slabs[i];
        if (slab->FreeObjects != 0) {
            VOID *object = slab->FreeObjects;
            UINT32 index;
            if (slab->FreeCount == 0U ||
                !slab_object_index(cache, slab, (UINT64)(uintptr_t)object,
                                    &index) || slab_object_allocated(slab, index)) {
                return 0;
            }
            slab->FreeObjects = *(VOID **)object;
            slab_set_object_allocated(slab, index, 1);
            --slab->FreeCount;
            return object;
        }
    }
    if (!add_slab(cache)) return 0;
    LITEOS_SLAB *slab = &cache->Slabs[cache->SlabCount - 1U];
    VOID *object = slab->FreeObjects;
    UINT32 index;
    if (object == 0 || slab->FreeCount == 0U ||
        !slab_object_index(cache, slab, (UINT64)(uintptr_t)object, &index) ||
        slab_object_allocated(slab, index)) return 0;
    slab->FreeObjects = *(VOID **)object;
    slab_set_object_allocated(slab, index, 1);
    --slab->FreeCount;
    return object;
}

BOOLEAN liteos_slab_free(LITEOS_SLAB_CACHE *cache, VOID *object) {
    if (cache == 0 || object == 0) return 0;
    UINT64 address = (UINT64)(uintptr_t)object;
    for (UINT32 i = 0; i < cache->SlabCount; ++i) {
        LITEOS_SLAB *slab = &cache->Slabs[i];
        UINT32 index;
        if (!slab_object_index(cache, slab, address, &index)) continue;
        /* 位图是释放状态的唯一判据，重复释放不得再次进入空闲链。 */
        if (!slab_object_allocated(slab, index) ||
            slab->FreeCount >= slab->ObjectCount) return 0;
        *(VOID **)object = slab->FreeObjects;
        slab->FreeObjects = object;
        slab_set_object_allocated(slab, index, 0);
        ++slab->FreeCount;
        return 1;
    }
    return 0;
}

VOID liteos_slab_cache_destroy(LITEOS_SLAB_CACHE *cache) {
    if (cache == 0) return;
    for (UINT32 i = 0; i < cache->SlabCount; ++i) {
        liteos_buddy_free(&cache->Slabs[i].PhysicalBlock);
    }
    cache->SlabCount = 0;
    cache->ObjectStride = 0;
}
