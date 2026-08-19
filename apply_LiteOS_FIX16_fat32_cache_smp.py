#!/usr/bin/env python3
from pathlib import Path

def load(path):
    p = Path(path)
    if not p.exists():
        raise SystemExit(f"missing {path}; run from LiteOS repository root")
    return p, p.read_text(encoding="utf-8")

def once(s, old, new, label):
    n = s.count(old)
    if n != 1:
        raise SystemExit(f"{label}: expected 1 match, found {n}")
    return s.replace(old, new, 1)

# include/cache.h
p, s = load("include/cache.h")
s = once(
    s,
'''typedef struct {
    const LITEOS_BLOCK_DEVICE *Device;
    LITEOS_BLOCK_CACHE_ENTRY Entries[LITEOS_BLOCK_CACHE_ENTRY_COUNT];
    UINT64 Clock;
    BOOLEAN Initialized;
} LITEOS_BLOCK_CACHE;
''',
'''typedef struct {
    const LITEOS_BLOCK_DEVICE *Device;
    LITEOS_BLOCK_CACHE_ENTRY Entries[LITEOS_BLOCK_CACHE_ENTRY_COUNT];
    UINT64 Clock;
    UINT32 Lock;
    BOOLEAN Initialized;
} LITEOS_BLOCK_CACHE;
''',
    "cache struct",
)
p.write_text(s, encoding="utf-8")

# kernel/fs/cache.c
p, old_cache = load("kernel/fs/cache.c")
new_cache = r'''#include "cache.h"

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
'''
p.write_text(new_cache, encoding="utf-8")

# include/fat32.h
p, s = load("include/fat32.h")
s = once(
    s,
'''    BOOLEAN Fat3Available;
    LITEOS_BLOCK_CACHE Cache;
    LITEOS_FAT32_FILE OpenFiles[LITEOS_FAT32_MAX_OPEN_FILES];
    BOOLEAN Mounted;
};
''',
'''    BOOLEAN Fat3Available;
    LITEOS_BLOCK_CACHE Cache;
    UINT32 OpenFileLock;
    LITEOS_FAT32_FILE OpenFiles[LITEOS_FAT32_MAX_OPEN_FILES];
    BOOLEAN Mounted;
};
''',
    "fat32 struct",
)
p.write_text(s, encoding="utf-8")

# kernel/fs/fat32.c
p, s = load("kernel/fs/fat32.c")

s = once(
    s,
'''static const LITEOS_VFS_FILE_OPERATIONS g_fat32_operations;

''',
'''static const LITEOS_VFS_FILE_OPERATIONS g_fat32_operations;

static VOID fat32_open_files_lock(LITEOS_FAT32 *filesystem) {
    while (__atomic_exchange_n(&filesystem->OpenFileLock, 1U,
                               __ATOMIC_ACQUIRE) != 0U) {
        __asm__ volatile ("pause");
    }
}

static VOID fat32_open_files_unlock(LITEOS_FAT32 *filesystem) {
    __atomic_store_n(&filesystem->OpenFileLock, 0U, __ATOMIC_RELEASE);
}

''',
    "fat32 lock helpers",
)

s = once(
    s,
'''static BOOLEAN fat32_file_open_at(const LITEOS_FAT32 *filesystem, UINT64 lba,
                                  UINT32 offset) {
    if (filesystem == 0) return 0;
    for (UINT32 i = 0U; i < LITEOS_FAT32_MAX_OPEN_FILES; ++i) {
        const LITEOS_FAT32_FILE *file = &filesystem->OpenFiles[i];
        if (file->Used && file->DirectoryLba == lba && file->DirectoryOffset == offset) return 1;
    }
    return 0;
}
''',
'''static BOOLEAN fat32_file_open_at(LITEOS_FAT32 *filesystem, UINT64 lba,
                                  UINT32 offset) {
    BOOLEAN found = 0;
    if (filesystem == 0) return 0;

    fat32_open_files_lock(filesystem);
    for (UINT32 i = 0U; i < LITEOS_FAT32_MAX_OPEN_FILES; ++i) {
        const LITEOS_FAT32_FILE *file = &filesystem->OpenFiles[i];
        if (file->Used && file->DirectoryLba == lba &&
            file->DirectoryOffset == offset) {
            found = 1;
            break;
        }
    }
    fat32_open_files_unlock(filesystem);
    return found;
}
''',
    "fat32_file_open_at",
)

s = once(
    s,
'''static BOOLEAN fat32_close(LITEOS_VFS_NODE *node) {
    LITEOS_FAT32_FILE *file = node == 0 ? 0 : (LITEOS_FAT32_FILE *)node->FileContext;
    if (file == 0 || !file->Used) return 0;
    file->Used = 0;
    return 1;
}
''',
'''static BOOLEAN fat32_close(LITEOS_VFS_NODE *node) {
    LITEOS_FAT32_FILE *file =
        node == 0 ? 0 : (LITEOS_FAT32_FILE *)node->FileContext;
    LITEOS_FAT32 *filesystem = file == 0 ? 0 : file->FileSystem;
    if (file == 0 || filesystem == 0) return 0;

    fat32_open_files_lock(filesystem);
    if (!file->Used) {
        fat32_open_files_unlock(filesystem);
        return 0;
    }
    file->Used = 0;
    file->CursorValid = 0;
    fat32_open_files_unlock(filesystem);
    return 1;
}
''',
    "fat32_close",
)

s = once(
    s,
'''    for (UINT32 i = 0; i < LITEOS_FAT32_MAX_OPEN_FILES; ++i) filesystem->OpenFiles[i].Used = 0;
    filesystem->Mounted = 1;
    return 1;
}
''',
'''    filesystem->OpenFileLock = 0U;
    for (UINT32 i = 0; i < LITEOS_FAT32_MAX_OPEN_FILES; ++i) {
        filesystem->OpenFiles[i].Used = 0;
    }
    filesystem->Mounted = 1;
    return 1;
}
''',
    "fat32 init",
)

old_lookup = '''BOOLEAN liteos_fat32_lookup(VOID *filesystem_context, const CHAR8 *path,
                            LITEOS_VFS_NODE *node) {
    LITEOS_FAT32 *filesystem = (LITEOS_FAT32 *)filesystem_context;
    UINT8 entry[32];
    UINT64 entry_lba = 0U;
    UINT32 entry_offset = 0U;
    if (filesystem == 0 || !filesystem->Mounted || path == 0 || node == 0 ||
        !resolve_path(filesystem, path, entry, &entry_lba, &entry_offset) ||
        (entry[11] & FAT32_DIRECTORY) != 0U) return 0;
    for (UINT32 i = 0; i < LITEOS_FAT32_MAX_OPEN_FILES; ++i) {
        LITEOS_FAT32_FILE *file = &filesystem->OpenFiles[i];
        if (file->Used) continue;
        file->Used = 1;
        file->FileSystem = filesystem;
        file->FirstCluster = entry_cluster(entry);
        file->Size = read_u32(entry + 28U);
        file->Attributes = entry[11];
        file->DirectoryLba = entry_lba;
        file->DirectoryOffset = entry_offset;
        file->CursorValid = 0;
        node->Type = 1U;
        node->Size = file->Size;
        node->FilesystemContext = filesystem;
        node->FileContext = file;
        node->SecurityDescriptor = 0;
        node->Operations = &g_fat32_operations;
        return 1;
    }
    return 0;
}
'''

new_lookup = '''BOOLEAN liteos_fat32_lookup(VOID *filesystem_context, const CHAR8 *path,
                            LITEOS_VFS_NODE *node) {
    LITEOS_FAT32 *filesystem = (LITEOS_FAT32 *)filesystem_context;
    UINT8 entry[32];
    UINT64 entry_lba = 0U;
    UINT32 entry_offset = 0U;

    if (filesystem == 0 || !filesystem->Mounted || path == 0 || node == 0 ||
        !resolve_path(filesystem, path, entry, &entry_lba, &entry_offset) ||
        (entry[11] & FAT32_DIRECTORY) != 0U) return 0;

    fat32_open_files_lock(filesystem);
    for (UINT32 i = 0; i < LITEOS_FAT32_MAX_OPEN_FILES; ++i) {
        LITEOS_FAT32_FILE *file = &filesystem->OpenFiles[i];
        if (file->Used) continue;

        file->FileSystem = filesystem;
        file->FirstCluster = entry_cluster(entry);
        file->Size = read_u32(entry + 28U);
        file->Attributes = entry[11];
        file->DirectoryLba = entry_lba;
        file->DirectoryOffset = entry_offset;
        file->CursorValid = 0;
        file->CursorLogicalStart = 0;
        file->CursorPhysicalStart = 0;
        file->CursorLength = 0;
        file->Used = 1;

        node->Type = 1U;
        node->Size = file->Size;
        node->FilesystemContext = filesystem;
        node->FileContext = file;
        node->SecurityDescriptor = 0;
        node->Operations = &g_fat32_operations;

        fat32_open_files_unlock(filesystem);
        return 1;
    }

    fat32_open_files_unlock(filesystem);
    return 0;
}
'''

s = once(s, old_lookup, new_lookup, "fat32 lookup")
p.write_text(s, encoding="utf-8")

print("FIX16-SMP applied")
print("  include/cache.h")
print("  kernel/fs/cache.c")
print("  include/fat32.h")
print("  kernel/fs/fat32.c")
print()
print("Run:")
print("  git diff --check")
print("  make clean && make")
print("  python3 tools/test_xhci_hub_hotplug.py")
