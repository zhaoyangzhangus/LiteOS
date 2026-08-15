#ifndef LITEOS_CACHE_H
#define LITEOS_CACHE_H

#include "block.h"

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
    BOOLEAN Initialized;
} LITEOS_BLOCK_CACHE;

/* 为一个块设备建立固定容量的页缓存。 */
BOOLEAN liteos_block_cache_init(LITEOS_BLOCK_CACHE *cache,
                                const LITEOS_BLOCK_DEVICE *device);

/* 读取一个块；命中缓存时不访问底层设备。 */
BOOLEAN liteos_block_cache_read(LITEOS_BLOCK_CACHE *cache, UINT64 lba,
                                VOID *buffer);

/* 写入一个完整块；数据先进入脏缓存，之后由 flush 回写。 */
BOOLEAN liteos_block_cache_write(LITEOS_BLOCK_CACHE *cache, UINT64 lba,
                                 const VOID *buffer);

/* 回写全部脏块，并同步底层设备。 */
BOOLEAN liteos_block_cache_flush(LITEOS_BLOCK_CACHE *cache);

/* 回写并销毁缓存。 */
BOOLEAN liteos_block_cache_destroy(LITEOS_BLOCK_CACHE *cache);

/* 使指定块失效；脏块会先回写。 */
BOOLEAN liteos_block_cache_invalidate(LITEOS_BLOCK_CACHE *cache, UINT64 lba);

#endif
