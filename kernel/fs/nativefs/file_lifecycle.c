#include <kernel/fat32.h>
#include "internal.h"

/* REFACTOR_FS_FAT32_FILE_LIFECYCLE_OWNER: file clusters, I/O and path handles. */

#define FAT32_EOC_MIN       0x0FFFFFF8U
#define FAT32_ENTRY_SIZE    32U

static BOOLEAN locate_file_cluster(OS_FAT32_FILE *file, UINT32 logical_cluster,
                                   UINT32 *physical_cluster) {
    OS_FAT32 *filesystem = file->FileSystem;
    UINT32 start = file->FirstCluster;
    UINT32 logical = 0;
    if (file->FirstCluster == 0U) return 0;
    /* The current extent already resolves this request; seek hints are only
     * useful when moving outside it. Subtract after the lower-bound check
     * so the upper-bound comparison cannot wrap. */
    if (file->CursorValid && logical_cluster >= file->CursorLogicalStart &&
        logical_cluster - file->CursorLogicalStart < file->CursorLength) {
        *physical_cluster = file->CursorPhysicalStart +
                            logical_cluster - file->CursorLogicalStart;
        return fat32_cluster_valid(filesystem, *physical_cluster);
    }
    /* Cursor invalidation on open/resize also invalidates all seek hints. */
    if (!file->CursorValid) {
        file->SeekCacheCount = 0;
        file->SeekCacheNext = 0;
        file->SeekIndexValid = 0U;
    } else {
        UINT32 hints = file->SeekIndexValid;
        while (hints != 0U) {
            UINT32 i = (UINT32)__builtin_ctz(hints);
            hints &= hints - 1U;
            UINT32 candidate = file->SeekIndexLogical[i];
            if (candidate <= logical_cluster &&
                (file->CursorLogicalStart > logical_cluster ||
                 candidate > file->CursorLogicalStart)) {
                file->CursorLogicalStart = candidate;
                file->CursorPhysicalStart = file->SeekIndexPhysical[i];
                file->CursorLength = file->SeekIndexLength[i];
            }
        }
        for (UINT32 i = 0; i < file->SeekCacheCount; ++i) {
            UINT32 candidate = file->SeekLogical[i];
            if (candidate <= logical_cluster &&
                (file->CursorLogicalStart > logical_cluster ||
                 candidate > file->CursorLogicalStart)) {
                file->CursorLogicalStart = candidate;
                file->CursorPhysicalStart = file->SeekPhysical[i];
                file->CursorLength = file->SeekLength[i];
            }
        }
    }

    /* Sequential reads should continue at the last resolved extent.  The old
     * code only used the cursor for an in-extent hit and restarted at
     * FirstCluster for every next extent, making a fragmented file O(n^2). */
    if (file->CursorValid && logical_cluster >= file->CursorLogicalStart) {
        UINT32 extent = file->CursorLength;
        start = file->CursorPhysicalStart;
        logical = file->CursorLogicalStart;
        if (extent != 0U && logical_cluster - logical < extent) {
            *physical_cluster = start + logical_cluster - logical;
            return fat32_cluster_valid(filesystem, *physical_cluster);
        }
        if (extent == 0U || logical > UINT32_MAX - extent) {
            file->CursorValid = 0;
            start = file->FirstCluster;
            logical = 0U;
        } else {
            logical += extent;
            if (!fat32_read_next_cluster(filesystem, start + extent - 1U, &start) ||
                start == 0U) return 0;
        }
    }

    /* Checkpoint spacing is needed only when walking the chain, not when a
     * current extent or seek hint resolves the request directly. */
    UINT64 cluster_bytes = (UINT64)filesystem->BytesPerSector *
                           filesystem->SectorsPerCluster;
    UINT64 clusters = cluster_bytes != 0U ?
        file->Size / cluster_bytes + (file->Size % cluster_bytes != 0U) :
        filesystem->ClusterCount;
    UINT64 stride = clusters / 32U + (clusters % 32U != 0U);
    if (stride == 0U) stride = 1U;
    for (UINT32 hops = 0; hops < filesystem->ClusterCount; ++hops) {
        UINT32 length = fat32_extent_length(filesystem, start);
        /* Keep the first visited extent in each file-sized interval. These
         * checkpoints survive hotspot-cache eviction without allocation. */
        UINT64 checkpoint = logical / stride;
        if (length != 0U && checkpoint < 32U &&
            (file->SeekIndexValid & (1U << (UINT32)checkpoint)) == 0U) {
            UINT32 slot = (UINT32)checkpoint;
            file->SeekIndexLogical[slot] = logical;
            file->SeekIndexPhysical[slot] = start;
            file->SeekIndexLength[slot] = length;
            file->SeekIndexValid |= 1U << slot;
        }
        if (logical_cluster >= logical && logical_cluster - logical < length) {
            file->CursorValid = 1;
            file->CursorLogicalStart = logical;
            file->CursorPhysicalStart = start;
            file->CursorLength = length;
            UINT32 slot = file->SeekCacheNext;
            file->SeekLogical[slot] = logical;
            file->SeekPhysical[slot] = start;
            file->SeekLength[slot] = length;
            file->SeekCacheNext = (slot + 1U) % 8U;
            if (file->SeekCacheCount < 8U) ++file->SeekCacheCount;
            *physical_cluster = start + logical_cluster - logical;
            return fat32_cluster_valid(filesystem, *physical_cluster);
        }
        UINT32 next;
        if (length == 0U || logical > UINT32_MAX - length ||
            !fat32_read_next_cluster(filesystem, start + length - 1U, &next) ||
            next == 0U) return 0;
        logical += length;
        start = next;
    }
    return 0;
}

static BOOLEAN count_file_clusters(const OS_FAT32 *filesystem, UINT32 first,
                                   UINT64 *count, UINT32 *last) {
    UINT32 cluster = first;
    UINT64 total = 0U;
    if (filesystem == 0 || count == 0 || first == 0U) {
        if (count != 0) *count = 0U;
        return first == 0U;
    }
    for (UINT32 hops = 0; hops < filesystem->ClusterCount; ++hops) {
        UINT32 length;
        UINT32 next;
        if (!fat32_cluster_valid(filesystem, cluster) ||
            (length = fat32_extent_length(filesystem, cluster)) == 0U ||
            total > UINT64_MAX - length) return 0;
        total += length;
        if (last != 0) *last = cluster + length - 1U;
        if (!fat32_read_next_cluster(filesystem, cluster + length - 1U, &next)) return 0;
        if (next == 0U) {
            *count = total;
            return 1;
        }
        cluster = next;
    }
    return 0;
}

static BOOLEAN update_file_directory_entry(OS_FAT32_FILE *file,
                                            UINT32 first_cluster, UINT64 size) {
    OS_FAT32 *filesystem = file == 0 ? 0 : file->FileSystem;
    UINT8 sector[4096];
    UINT8 *entry;
    if (filesystem == 0 || filesystem->BytesPerSector > sizeof(sector) ||
        file->DirectoryOffset > filesystem->BytesPerSector - FAT32_ENTRY_SIZE ||
        size > UINT32_MAX || !fat32_read_sector(filesystem, file->DirectoryLba, sector)) {
        return 0;
    }
    entry = sector + file->DirectoryOffset;
    fat32_write_u16(entry + 20U, (UINT16)(first_cluster >> 16));
    fat32_write_u16(entry + 26U, (UINT16)first_cluster);
    fat32_write_u32(entry + 28U, (UINT32)size);
    return fat32_write_sector(filesystem, file->DirectoryLba, sector);
}

static BOOLEAN extend_file_to(OS_FAT32_FILE *file, UINT64 required_size) {
    OS_FAT32 *filesystem = file == 0 ? 0 : file->FileSystem;
    UINT64 cluster_bytes;
    UINT64 required_clusters;
    UINT64 existing_clusters = 0U;
    UINT32 old_last = 0U;
    UINT32 new_first = 0U;
    UINT32 new_last = 0U;
    UINT32 orphan = 0U;
    UINT32 scan_start = 2U;
    UINT32 old_first;
    BOOLEAN attached = 0;
    if (filesystem == 0 || required_size <= file->Size) return 1;
    cluster_bytes = (UINT64)filesystem->BytesPerSector *
                    filesystem->SectorsPerCluster;
    if (cluster_bytes == 0U || required_size > UINT64_MAX - (cluster_bytes - 1U)) {
        return 0;
    }
    required_clusters = (required_size + cluster_bytes - 1U) / cluster_bytes;
    old_first = file->FirstCluster;
    if (!count_file_clusters(filesystem, old_first, &existing_clusters, &old_last)) {
        return 0;
    }
    if (required_clusters <= existing_clusters) {
        file->Size = required_size;
        file->CursorValid = 0;
        return update_file_directory_entry(file, old_first, required_size);
    }
    if (required_clusters > filesystem->ClusterCount + 1ULL) return 0;

    for (UINT64 index = existing_clusters; index < required_clusters; ++index) {
        UINT32 cluster;
        if (!fat32_find_free_cluster_from(filesystem, scan_start, &cluster)) goto fail;
        orphan = cluster;
        if (!fat32_write_file_fat_value(filesystem, cluster, FAT32_EOC_MIN, 1U)) goto fail;
        orphan = 0U;
        if (new_first == 0U) {
            new_first = cluster;
        } else if (!fat32_write_file_fat_value(filesystem, new_last, cluster, 1U)) {
            orphan = cluster;
            goto fail;
        }
        new_last = cluster;
        /* Earlier entries are occupied for this mutation-locked transaction.
         * Do not persist the hint: rollback/deletion can free them afterward. */
        scan_start = cluster + 1U;
    }
    if (old_last != 0U) {
        if (!fat32_write_file_fat_value(filesystem, old_last, new_first, 1U)) goto fail;
        attached = 1;
    } else {
        old_first = new_first;
    }
    if (!update_file_directory_entry(file, old_first, required_size)) goto fail;
    file->FirstCluster = old_first;
    file->Size = required_size;
    file->CursorValid = 0;
    return 1;

fail:
    if (attached) (void)fat32_write_file_fat_value(filesystem, old_last,
                                               FAT32_EOC_MIN, 1U);
    if (new_first != 0U) (void)fat32_free_cluster_chain(filesystem, new_first);
    if (orphan != 0U) (void)fat32_write_file_fat_value(filesystem, orphan, 0U, 0U);
    return 0;
}

static BOOLEAN truncate_file_to(OS_FAT32_FILE *file, UINT64 required_size) {
    OS_FAT32 *filesystem = file == 0 ? 0 : file->FileSystem;
    UINT64 cluster_bytes;
    UINT64 required_clusters;
    UINT64 existing_clusters = 0U;
    UINT32 old_last = 0U;
    UINT32 new_last;
    UINT32 tail = 0U;
    if (filesystem == 0 || required_size > UINT32_MAX || required_size > file->Size) {
        return required_size > file->Size ? extend_file_to(file, required_size) : 0;
    }
    cluster_bytes = (UINT64)filesystem->BytesPerSector * filesystem->SectorsPerCluster;
    if (cluster_bytes == 0U || required_size > UINT64_MAX - (cluster_bytes - 1U) ||
        !count_file_clusters(filesystem, file->FirstCluster, &existing_clusters,
                              &old_last)) return 0;
    required_clusters = required_size == 0U ? 0U :
        (required_size + cluster_bytes - 1U) / cluster_bytes;
    if (required_clusters >= existing_clusters) {
        file->Size = required_size;
        file->CursorValid = 0;
        return update_file_directory_entry(file, file->FirstCluster, required_size);
    }
    if (required_clusters == 0U) {
        if (!update_file_directory_entry(file, 0U, 0U) ||
            !fat32_free_cluster_chain(filesystem, file->FirstCluster)) return 0;
        file->FirstCluster = 0U;
    } else {
        if (!locate_file_cluster(file, (UINT32)(required_clusters - 1U), &new_last) ||
            !fat32_read_next_cluster(filesystem, new_last, &tail) ||
            !fat32_write_file_fat_value(filesystem, new_last, FAT32_EOC_MIN, 1U) ||
            (tail != 0U && !fat32_free_cluster_chain(filesystem, tail)) ||
            !update_file_directory_entry(file, file->FirstCluster, required_size)) {
            return 0;
        }
    }
    file->Size = required_size;
    file->CursorValid = 0;
    return 1;
}

static BOOLEAN fat32_read(OS_FAT32_FILE *file, UINT64 offset, VOID *buffer,
                          UINT32 capacity, UINT32 *read_size) {
    OS_FAT32 *filesystem = file == 0 ? 0 : file->FileSystem;
    UINT8 sector[4096];
    UINT64 remaining;
    UINT8 *destination = (UINT8 *)buffer;
    if (file == 0 || filesystem == 0 || !file->Used || buffer == 0 || read_size == 0 ||
        capacity == 0U || offset > file->Size || filesystem->BytesPerSector > sizeof(sector)) {
        fat32_diag_failure("FAT_READ_ARGS_FAIL", offset, capacity,
                           file != 0 ? file->Size : 0U);
        return 0;
    }
    remaining = file->Size - offset;
    if (remaining > capacity) remaining = capacity;
    *read_size = 0;
    while ((UINT64)*read_size < remaining) {
        UINT64 current = offset + *read_size;
        UINT32 cluster_bytes = filesystem->BytesPerSector * filesystem->SectorsPerCluster;
        UINT32 logical_cluster = (UINT32)(current / cluster_bytes);
        UINT32 physical_cluster;
        UINT32 in_cluster = (UINT32)(current % cluster_bytes);
        UINT32 sector_offset = in_cluster % filesystem->BytesPerSector;
        UINT32 copy_size;
        if (!locate_file_cluster(file, logical_cluster, &physical_cluster)) {
            fat32_diag_failure("FAT_LOCATE_FAIL", current, logical_cluster,
                               file->FirstCluster);
            return 0;
        }
        UINT64 sector_lba = fat32_cluster_lba(filesystem, physical_cluster) +
                            in_cluster / filesystem->BytesPerSector;
        /* Small reads retain normal cache admission; stream only requests
         * large enough to fill the entire sector cache. */
        if (sector_offset == 0U && remaining - *read_size >=
                (UINT64)OS_BLOCK_CACHE_ENTRY_COUNT * filesystem->BytesPerSector) {
            UINT64 extent_bytes = cluster_bytes - in_cluster;
            if (file->CursorValid && logical_cluster >= file->CursorLogicalStart &&
                logical_cluster - file->CursorLogicalStart < file->CursorLength) {
                extent_bytes = (UINT64)(file->CursorLength -
                    (logical_cluster - file->CursorLogicalStart)) * cluster_bytes -
                    in_cluster;
            }
            if (extent_bytes > remaining - *read_size)
                extent_bytes = remaining - *read_size;
            UINT64 sectors = extent_bytes / filesystem->BytesPerSector;
            if (sectors > 128U) sectors = 128U;
            if (sectors >= OS_BLOCK_CACHE_ENTRY_COUNT) {
                BOOLEAN success = 0;
                for (UINT32 attempt = 0U; attempt < 3U && !success; ++attempt) {
                    success = filesystem->Cache.Initialized ?
                        block_cache_read_range(&filesystem->Cache,
                            sector_lba, (UINT32)sectors, destination + *read_size) :
                        block_read(filesystem->Device, sector_lba,
                            (UINT32)sectors, destination + *read_size);
                }
                if (!success) {
                    fat32_diag_failure("FAT_DATA_RANGE_FAIL", sector_lba,
                                       sectors, physical_cluster);
                    return 0;
                }
                *read_size += (UINT32)sectors * filesystem->BytesPerSector;
                continue;
            }
        }
        if (!fat32_read_sector(filesystem, sector_lba, sector)) {
            fat32_diag_failure("FAT_DATA_SECTOR_FAIL", sector_lba,
                               logical_cluster, physical_cluster);
            return 0;
        }
        copy_size = filesystem->BytesPerSector - sector_offset;
        if (copy_size > remaining - *read_size) copy_size = (UINT32)(remaining - *read_size);
        __builtin_memcpy(destination + *read_size, sector + sector_offset, copy_size);
        *read_size += copy_size;
    }
    return 1;
}

static BOOLEAN fat32_write_locked(OS_FAT32_FILE *file, UINT64 offset,
                                  const VOID *buffer, UINT32 size,
                                  UINT32 *written_size) {
    OS_FAT32 *filesystem = file == 0 ? 0 : file->FileSystem;
    UINT8 sector[4096];
    const UINT8 *source = (const UINT8 *)buffer;
    if (file == 0 || filesystem == 0 || !file->Used || buffer == 0 || written_size == 0 ||
        offset > UINT64_MAX - size || filesystem->BytesPerSector > sizeof(sector)) return 0;
    *written_size = 0;
    if (!extend_file_to(file, offset + size)) return 0;
    while (*written_size < size) {
        UINT64 current = offset + *written_size;
        UINT32 cluster_bytes = filesystem->BytesPerSector * filesystem->SectorsPerCluster;
        UINT32 logical_cluster = (UINT32)(current / cluster_bytes);
        UINT32 physical_cluster;
        UINT32 in_cluster = (UINT32)(current % cluster_bytes);
        UINT32 sector_offset = in_cluster % filesystem->BytesPerSector;
        UINT32 copy_size = filesystem->BytesPerSector - sector_offset;
        UINT64 lba;
        if (copy_size > size - *written_size) copy_size = size - *written_size;
        if (!locate_file_cluster(file, logical_cluster, &physical_cluster)) return 0;
        lba = fat32_cluster_lba(filesystem, physical_cluster) +
              in_cluster / filesystem->BytesPerSector;
        /* A complete sector overwrite does not need a read-modify-write cycle. */
        if ((sector_offset != 0U || copy_size != filesystem->BytesPerSector) &&
            !fat32_read_sector(filesystem, lba, sector)) return 0;
        __builtin_memcpy(sector + sector_offset, source + *written_size, copy_size);
        if (!block_cache_write(&filesystem->Cache, lba, sector)) return 0;
        *written_size += copy_size;
    }
    return 1;
}

static BOOLEAN fat32_write(OS_FAT32_FILE *file, UINT64 offset,
                           const VOID *buffer, UINT32 size,
                           UINT32 *written_size) {
    OS_FAT32 *filesystem = file == 0 ? 0 : file->FileSystem;
    BOOLEAN success;
    if (filesystem == 0) return 0;

    fat32_mutation_lock(filesystem);
    success = fat32_write_locked(file, offset, buffer, size, written_size);
    fat32_mutation_unlock(filesystem);
    return success;
}

BOOLEAN fat32_close(OS_FAT32_FILE *file) {
    OS_FAT32 *filesystem = file == 0 ? 0 : file->FileSystem;
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

BOOLEAN fat32_read_path(OS_FAT32 *filesystem, const CHAR8 *path,
                                UINT64 offset, VOID *buffer, UINT32 capacity,
                                UINT32 *read_size) {
    OS_FAT32_FILE *file = 0;
    BOOLEAN success;
    if (filesystem == 0 || path == 0 || read_size == 0) return 0;
    if (capacity == 0U) {
        *read_size = 0;
        return 1;
    }
    if (!fat32_open(filesystem, path, &file)) return 0;
    success = fat32_read(file, offset, buffer, capacity, read_size);
    if (!fat32_close(file)) success = 0;
    return success;
}

BOOLEAN fat32_read_file(OS_FAT32_FILE *file, UINT64 offset,
                               VOID *buffer, UINT32 capacity,
                               UINT32 *read_size) {
    OS_FAT32 *filesystem = file == 0 ? 0 : file->FileSystem;
    if (filesystem == 0 || read_size == 0) return 0;
    return fat32_read(file, offset, buffer, capacity, read_size);
}

BOOLEAN fat32_write_path(OS_FAT32 *filesystem, const CHAR8 *path,
                                 UINT64 offset, const VOID *buffer, UINT32 size,
                                 UINT32 *written_size) {
    OS_FAT32_FILE *file = 0;
    BOOLEAN success;
    if (filesystem == 0 || path == 0 || written_size == 0) return 0;
    if (size == 0U) {
        *written_size = 0;
        return 1;
    }
    if (!fat32_open(filesystem, path, &file)) return 0;
    success = fat32_write(file, offset, buffer, size, written_size);
    if (!fat32_close(file)) success = 0;
    return success;
}

BOOLEAN fat32_write_file(OS_FAT32_FILE *file, UINT64 offset,
                                const VOID *buffer, UINT32 size,
                                UINT32 *written_size) {
    OS_FAT32 *filesystem = file == 0 ? 0 : file->FileSystem;
    if (filesystem == 0 || written_size == 0) return 0;
    return fat32_write(file, offset, buffer, size, written_size);
}

static BOOLEAN fat32_truncate_path_locked(OS_FAT32 *filesystem,
                                           const CHAR8 *path, UINT64 size) {
    OS_FAT32_FILE *file = 0;
    BOOLEAN success;
    if (filesystem == 0 || path == 0 || size > UINT32_MAX ||
        !fat32_open(filesystem, path, &file)) return 0;
    success = truncate_file_to(file, size);
    if (!fat32_close(file)) success = 0;
    return success;
}

BOOLEAN fat32_truncate_file(OS_FAT32_FILE *file, UINT64 size) {
    OS_FAT32 *filesystem = file == 0 ? 0 : file->FileSystem;
    if (filesystem == 0 || !file->Used || size > UINT32_MAX) return 0;
    fat32_mutation_lock(filesystem);
    BOOLEAN success = fat32_writeback(filesystem);
    if (success) success = truncate_file_to(file, size);
    if (success) success = fat32_writeback(filesystem);
    fat32_mutation_unlock(filesystem);
    return success;
}

BOOLEAN fat32_truncate_path(OS_FAT32 *filesystem, const CHAR8 *path,
                                    UINT64 size) {
    BOOLEAN success;
    if (filesystem == 0) return 0;
    fat32_mutation_lock(filesystem);
    success = fat32_writeback(filesystem);
    if (success) {
        success = fat32_truncate_path_locked(filesystem, path, size);
        if (success) success = fat32_writeback(filesystem);
    }
    fat32_mutation_unlock(filesystem);
    return success;
}
