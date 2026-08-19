#include "cache.h"

static VOID copy_bytes(UINT8 *destination, const UINT8 *source, UINT32 size) {
    for (UINT32 i = 0; i < size; ++i) destination[i] = source[i];
}

static VOID cache_lock(LITEOS_BLOCK_CACHE *cache) {
    while (__atomic_exchange_n(&cache->Lock, 1U, __ATOMIC_ACQUIRE) != 0U) {
        __asm__ volatile ("pause");
    }
}

static VOID cache_unlock(LITEOS_BLOCK_CACHE *cache) {
    __atomic_store_n(&cache->Lock, 0U, __ATOMIC_RELEASE);
}

static LITEOS_BLOCK_CACHE_ENTRY *find_entry_locked(LITEOS_BLOCK_CACHE *cache,
                                                   UINT64 lba) {
    for (UINT32 i = 0; i < LITEOS_BLOCK_CACHE_ENTRY_COUNT; ++i) {
        LITEOS_BLOCK_CACHE_ENTRY *entry = &cache->Entries[i];
        if (entry->Valid && entry->Lba == lba) return entry;
    }
    return 0;
}

static BOOLEAN flush_entry_locked(LITEOS_BLOCK_CACHE *cache,
                                  LITEOS_BLOCK_CACHE_ENTRY *entry) {
    if (!entry->Valid || !entry->Dirty) return 1;
    if (!liteos_block_write(cache->Device, entry->Lba, 1U, entry->Data)) return 0;
    entry->Dirty = 0;
    return 1;
}

static LITEOS_BLOCK_CACHE_ENTRY *select_entry_locked(LITEOS_BLOCK_CACHE *cache) {
    LITEOS_BLOCK_CACHE_ENTRY *selected = 0;
    for (UINT32 i = 0; i < LITEOS_BLOCK_CACHE_ENTRY_COUNT; ++i) {
        LITEOS_BLOCK_CACHE_ENTRY *entry = &cache->Entries[i];
        if (!entry->Valid) return entry;
        if (selected == 0 || entry->LastUse < selected->LastUse) selected = entry;
    }
    return selected;
}

static BOOLEAN load_entry_locked(LITEOS_BLOCK_CACHE *cache, UINT64 lba,
                                 LITEOS_BLOCK_CACHE_ENTRY **result) {
    LITEOS_BLOCK_CACHE_ENTRY *entry;
    if (cache == 0 || result == 0 || !cache->Initialized ||
        cache->Device == 0 || cache->Device->BlockSize > LITEOS_BLOCK_CACHE_MAX_SIZE ||
        lba >= cache->Device->BlockCount) return 0;

    entry = find_entry_locked(cache, lba);
    if (entry == 0) {
        entry = select_entry_locked(cache);
        if (entry == 0 || !flush_entry_locked(cache, entry) ||
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

static BOOLEAN flush_cache_locked(LITEOS_BLOCK_CACHE *cache) {
    for (UINT32 i = 0; i < LITEOS_BLOCK_CACHE_ENTRY_COUNT; ++i) {
        if (!flush_entry_locked(cache, &cache->Entries[i])) return 0;
    }
    return liteos_block_flush(cache->Device);
}

BOOLEAN liteos_block_cache_init(LITEOS_BLOCK_CACHE *cache,
                                const LITEOS_BLOCK_DEVICE *device) {
    if (cache == 0 || device == 0 || !device->Registered || device->BlockSize == 0U ||
        device->BlockSize > LITEOS_BLOCK_CACHE_MAX_SIZE || cache->Initialized) return 0;

    cache->Device = device;
    cache->Clock = 0;
    cache->Lock = 0U;
    for (UINT32 i = 0; i < LITEOS_BLOCK_CACHE_ENTRY_COUNT; ++i) {
        cache->Entries[i].Valid = 0;
        cache->Entries[i].Dirty = 0;
        cache->Entries[i].Lba = 0;
        cache->Entries[i].LastUse = 0;
    }
    __atomic_store_n(&cache->Initialized, 1, __ATOMIC_RELEASE);
    return 1;
}

BOOLEAN liteos_block_cache_read(LITEOS_BLOCK_CACHE *cache, UINT64 lba,
                                VOID *buffer) {
    LITEOS_BLOCK_CACHE_ENTRY *entry;
    BOOLEAN success = 0;

    if (cache == 0 || buffer == 0) return 0;
    cache_lock(cache);
    if (load_entry_locked(cache, lba, &entry)) {
        copy_bytes((UINT8 *)buffer, entry->Data, cache->Device->BlockSize);
        success = 1;
    }
    cache_unlock(cache);
    return success;
}

BOOLEAN liteos_block_cache_write(LITEOS_BLOCK_CACHE *cache, UINT64 lba,
                                 const VOID *buffer) {
    LITEOS_BLOCK_CACHE_ENTRY *entry;

    if (cache == 0 || buffer == 0) return 0;
    cache_lock(cache);

    if (!cache->Initialized || cache->Device == 0 ||
        cache->Device->BlockSize > LITEOS_BLOCK_CACHE_MAX_SIZE ||
        lba >= cache->Device->BlockCount) {
        cache_unlock(cache);
        return 0;
    }

    entry = find_entry_locked(cache, lba);
    if (entry == 0) {
        entry = select_entry_locked(cache);
        if (entry == 0 || !flush_entry_locked(cache, entry)) {
            cache_unlock(cache);
            return 0;
        }
        entry->Valid = 1;
        entry->Dirty = 0;
        entry->Lba = lba;
    }

    ++cache->Clock;
    entry->LastUse = cache->Clock;
    copy_bytes(entry->Data, (const UINT8 *)buffer, cache->Device->BlockSize);
    entry->Dirty = 1;

    cache_unlock(cache);
    return 1;
}

BOOLEAN liteos_block_cache_flush(LITEOS_BLOCK_CACHE *cache) {
    BOOLEAN success;
    if (cache == 0) return 0;

    cache_lock(cache);
    if (!cache->Initialized || cache->Device == 0) {
        cache_unlock(cache);
        return 0;
    }

    success = flush_cache_locked(cache);
    cache_unlock(cache);
    return success;
}

BOOLEAN liteos_block_cache_destroy(LITEOS_BLOCK_CACHE *cache) {
    if (cache == 0) return 0;

    cache_lock(cache);
    if (!cache->Initialized || cache->Device == 0 || !flush_cache_locked(cache)) {
        cache_unlock(cache);
        return 0;
    }

    cache->Initialized = 0;
    cache->Device = 0;
    cache->Clock = 0;
    for (UINT32 i = 0; i < LITEOS_BLOCK_CACHE_ENTRY_COUNT; ++i) {
        cache->Entries[i].Valid = 0;
        cache->Entries[i].Dirty = 0;
        cache->Entries[i].Lba = 0;
        cache->Entries[i].LastUse = 0;
    }

    cache_unlock(cache);
    return 1;
}

BOOLEAN liteos_block_cache_invalidate(LITEOS_BLOCK_CACHE *cache, UINT64 lba) {
    LITEOS_BLOCK_CACHE_ENTRY *entry;
    if (cache == 0) return 0;

    cache_lock(cache);
    if (!cache->Initialized) {
        cache_unlock(cache);
        return 0;
    }

    entry = find_entry_locked(cache, lba);
    if (entry == 0) {
        cache_unlock(cache);
        return 1;
    }
    if (!flush_entry_locked(cache, entry)) {
        cache_unlock(cache);
        return 0;
    }

    entry->Valid = 0;
    entry->Dirty = 0;
    entry->Lba = 0;
    entry->LastUse = 0;

    cache_unlock(cache);
    return 1;
}
