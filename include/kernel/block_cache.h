#pragma once

#include <kernel/block_device.h>
#include <kernel/mutex.h>

#define OS_BLOCK_CACHE_ENTRY_COUNT 16U
#define OS_BLOCK_CACHE_MAX_SIZE    4096U

typedef struct {
    BOOLEAN Valid;
    BOOLEAN Dirty;
    UINT64 Lba;
    UINT64 LastUse;
    UINT8 Data[OS_BLOCK_CACHE_MAX_SIZE];
} OS_BLOCK_CACHE_ENTRY;

typedef struct {
    const OS_BLOCK_DEVICE *Device;
    OS_BLOCK_CACHE_ENTRY Entries[OS_BLOCK_CACHE_ENTRY_COUNT];
    UINT64 Clock;
    /* Scheduled callers may sleep while the backend services a disk I/O. */
    kmutex_t WaitLock;
    /* Single-threaded early boot / bootstrap-idle fallback. */
    UINT32 Lock;
    BOOLEAN Initialized;
} OS_BLOCK_CACHE;

BOOLEAN block_cache_init(OS_BLOCK_CACHE *cache,
                                const OS_BLOCK_DEVICE *device);
BOOLEAN block_cache_read(OS_BLOCK_CACHE *cache, UINT64 lba,
                                VOID *buffer);
/* Stream a bounded extent, honoring both clean and dirty cached sectors. */
BOOLEAN block_cache_read_range(OS_BLOCK_CACHE *cache, UINT64 lba,
                                      UINT32 count, VOID *buffer);
BOOLEAN block_cache_write(OS_BLOCK_CACHE *cache, UINT64 lba,
                                 const VOID *buffer);
/* Write dirty sectors to the device without issuing a device cache flush. */
BOOLEAN block_cache_writeback(OS_BLOCK_CACHE *cache);
BOOLEAN block_cache_flush(OS_BLOCK_CACHE *cache);
BOOLEAN block_cache_destroy(OS_BLOCK_CACHE *cache);
BOOLEAN block_cache_invalidate(OS_BLOCK_CACHE *cache, UINT64 lba);
