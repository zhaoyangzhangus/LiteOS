#include "cache.h"

static VOID copy_bytes(UINT8 *destination, const UINT8 *source, UINT32 size) {
    for (UINT32 i = 0; i < size; ++i) destination[i] = source[i];
}

static LITEOS_BLOCK_CACHE_ENTRY *find_entry(LITEOS_BLOCK_CACHE *cache,
                                            UINT64 lba) {
    for (UINT32 i = 0; i < LITEOS_BLOCK_CACHE_ENTRY_COUNT; ++i) {
        LITEOS_BLOCK_CACHE_ENTRY *entry = &cache->Entries[i];
        if (entry->Valid && entry->Lba == lba) return entry;
    }
    return 0;
}

static BOOLEAN flush_entry(LITEOS_BLOCK_CACHE *cache,
                           LITEOS_BLOCK_CACHE_ENTRY *entry) {
    if (!entry->Valid || !entry->Dirty) return 1;
    if (!liteos_block_write(cache->Device, entry->Lba, 1U, entry->Data)) return 0;
    entry->Dirty = 0;
    return 1;
}

static LITEOS_BLOCK_CACHE_ENTRY *select_entry(LITEOS_BLOCK_CACHE *cache) {
    LITEOS_BLOCK_CACHE_ENTRY *selected = 0;
    for (UINT32 i = 0; i < LITEOS_BLOCK_CACHE_ENTRY_COUNT; ++i) {
        LITEOS_BLOCK_CACHE_ENTRY *entry = &cache->Entries[i];
        if (!entry->Valid) return entry;
        if (selected == 0 || entry->LastUse < selected->LastUse) selected = entry;
    }
    return selected;
}

static BOOLEAN load_entry(LITEOS_BLOCK_CACHE *cache, UINT64 lba,
                          LITEOS_BLOCK_CACHE_ENTRY **result) {
    LITEOS_BLOCK_CACHE_ENTRY *entry;
    if (cache == 0 || result == 0 || !cache->Initialized ||
        cache->Device == 0 || cache->Device->BlockSize > LITEOS_BLOCK_CACHE_MAX_SIZE ||
        lba >= cache->Device->BlockCount) return 0;
    entry = find_entry(cache, lba);
    if (entry == 0) {
        entry = select_entry(cache);
        if (entry == 0 || !flush_entry(cache, entry) ||
            !liteos_block_read(cache->Device, lba, 1U, entry->Data)) return 0;
        entry->Valid = 1;
        entry->Dirty = 0;
        entry->Lba = lba;
    }
    ++cache->Clock;
    entry->LastUse = cache->Clock;
    *result = entry;
    return 1;
}

BOOLEAN liteos_block_cache_init(LITEOS_BLOCK_CACHE *cache,
                                const LITEOS_BLOCK_DEVICE *device) {
    if (cache == 0 || device == 0 || !device->Registered || device->BlockSize == 0U ||
        device->BlockSize > LITEOS_BLOCK_CACHE_MAX_SIZE || cache->Initialized) return 0;
    cache->Device = device;
    cache->Clock = 0;
    for (UINT32 i = 0; i < LITEOS_BLOCK_CACHE_ENTRY_COUNT; ++i) {
        cache->Entries[i].Valid = 0;
        cache->Entries[i].Dirty = 0;
        cache->Entries[i].Lba = 0;
        cache->Entries[i].LastUse = 0;
    }
    cache->Initialized = 1;
    return 1;
}

BOOLEAN liteos_block_cache_read(LITEOS_BLOCK_CACHE *cache, UINT64 lba,
                                VOID *buffer) {
    LITEOS_BLOCK_CACHE_ENTRY *entry;
    if (buffer == 0 || !load_entry(cache, lba, &entry)) return 0;
    copy_bytes((UINT8 *)buffer, entry->Data, cache->Device->BlockSize);
    return 1;
}

BOOLEAN liteos_block_cache_write(LITEOS_BLOCK_CACHE *cache, UINT64 lba,
                                 const VOID *buffer) {
    LITEOS_BLOCK_CACHE_ENTRY *entry;
    if (cache == 0 || buffer == 0 || !cache->Initialized || cache->Device == 0 ||
        cache->Device->BlockSize > LITEOS_BLOCK_CACHE_MAX_SIZE ||
        lba >= cache->Device->BlockCount) return 0;
    entry = find_entry(cache, lba);
    if (entry == 0) {
        /*
         * This API always replaces a complete block.  Reading the old block on
         * a cache miss is therefore unnecessary I/O; only an evicted dirty
         * entry needs to be written back before its slot is reused.
         */
        entry = select_entry(cache);
        if (entry == 0 || !flush_entry(cache, entry)) return 0;
        entry->Valid = 1;
        entry->Dirty = 0;
        entry->Lba = lba;
    }
    ++cache->Clock;
    entry->LastUse = cache->Clock;
    copy_bytes(entry->Data, (const UINT8 *)buffer, cache->Device->BlockSize);
    entry->Dirty = 1;
    return 1;
}

BOOLEAN liteos_block_cache_flush(LITEOS_BLOCK_CACHE *cache) {
    if (cache == 0 || !cache->Initialized || cache->Device == 0) return 0;
    for (UINT32 i = 0; i < LITEOS_BLOCK_CACHE_ENTRY_COUNT; ++i) {
        if (!flush_entry(cache, &cache->Entries[i])) return 0;
    }
    return liteos_block_flush(cache->Device);
}

BOOLEAN liteos_block_cache_destroy(LITEOS_BLOCK_CACHE *cache) {
    if (cache == 0 || !cache->Initialized || !liteos_block_cache_flush(cache)) return 0;
    cache->Device = 0;
    cache->Clock = 0;
    cache->Initialized = 0;
    for (UINT32 i = 0; i < LITEOS_BLOCK_CACHE_ENTRY_COUNT; ++i) {
        cache->Entries[i].Valid = 0;
        cache->Entries[i].Dirty = 0;
    }
    return 1;
}

BOOLEAN liteos_block_cache_invalidate(LITEOS_BLOCK_CACHE *cache, UINT64 lba) {
    LITEOS_BLOCK_CACHE_ENTRY *entry;
    if (cache == 0 || !cache->Initialized || (entry = find_entry(cache, lba)) == 0) return 1;
    if (!flush_entry(cache, entry)) return 0;
    entry->Valid = 0;
    entry->LastUse = 0;
    return 1;
}
