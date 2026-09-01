#pragma once

#include <kernel/block_device.h>

#define LITEOS_BLOCK_CACHE_ENTRY_COUNT 16U
#define LITEOS_BLOCK_CACHE_MAX_SIZE    4096U

typedef struct {
    BOOLEAN Valid;
    BOOLEAN Dirty;
    UINT64 Lba;
    UINT64 LastUse;
    UINT8 Data[LITEOS_BLOCK_CACHE_MAX_SIZE];
} LITEOS_BLOCK_CACHE_ENTRY;

typedef struct {
    const LITEOS_BLOCK_DEVICE *Device;
    LITEOS_BLOCK_CACHE_ENTRY Entries[LITEOS_BLOCK_CACHE_ENTRY_COUNT];
    UINT64 Clock;
    UINT32 Lock;
    BOOLEAN Initialized;
} LITEOS_BLOCK_CACHE;

BOOLEAN liteos_block_cache_init(LITEOS_BLOCK_CACHE *cache,
                                const LITEOS_BLOCK_DEVICE *device);
BOOLEAN liteos_block_cache_read(LITEOS_BLOCK_CACHE *cache, UINT64 lba,
                                VOID *buffer);
BOOLEAN liteos_block_cache_write(LITEOS_BLOCK_CACHE *cache, UINT64 lba,
                                 const VOID *buffer);
/* Write dirty sectors to the device without issuing a device cache flush. */
BOOLEAN liteos_block_cache_writeback(LITEOS_BLOCK_CACHE *cache);
BOOLEAN liteos_block_cache_flush(LITEOS_BLOCK_CACHE *cache);
BOOLEAN liteos_block_cache_destroy(LITEOS_BLOCK_CACHE *cache);
BOOLEAN liteos_block_cache_invalidate(LITEOS_BLOCK_CACHE *cache, UINT64 lba);
