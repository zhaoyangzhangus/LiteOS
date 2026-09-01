#include <kernel/fat32.h>
#include "internal.h"

/* REFACTOR_FS_FAT32_DIRECTORY_LIFECYCLE_OWNER: path and directory mutation state. */

#define FAT32_EOC_MIN       0x0FFFFFF8U
#define FAT32_DIRECTORY     0x10U
#define FAT32_ENTRY_SIZE    32U
#define FAT32_ATTRIBUTE_LFN 0x0FU
#define FAT32_MAX_LFN_ENTRIES 20U
#define FAT32_MAX_CREATE_SLOTS (FAT32_MAX_LFN_ENTRIES + 1U)

VOID fat32_open_files_lock(LITEOS_FAT32 *filesystem) {
    while (__atomic_exchange_n(&filesystem->OpenFileLock, 1U,
                               __ATOMIC_ACQUIRE) != 0U) {
        __asm__ volatile ("pause");
    }
}

VOID fat32_open_files_unlock(LITEOS_FAT32 *filesystem) {
    __atomic_store_n(&filesystem->OpenFileLock, 0U, __ATOMIC_RELEASE);
}

VOID fat32_rebind_open_file(LITEOS_FAT32 *filesystem, UINT64 old_lba,
                            UINT32 old_offset, UINT64 new_lba,
                            UINT32 new_offset) {
    if (filesystem == 0) return;
    fat32_open_files_lock(filesystem);
    for (UINT32 index = 0U; index < LITEOS_FAT32_MAX_OPEN_FILES; ++index) {
        LITEOS_FAT32_FILE *file = &filesystem->OpenFiles[index];
        if (file->Used && file->DirectoryLba == old_lba &&
            file->DirectoryOffset == old_offset) {
            file->DirectoryLba = new_lba;
            file->DirectoryOffset = new_offset;
        }
    }
    fat32_open_files_unlock(filesystem);
}

/*
 * The block-cache lock only protects one cached sector.  FAT allocation and
 * directory updates are multi-sector transactions, so writers need a
 * filesystem-wide mutation lock to keep two threads from allocating or
 * linking the same free cluster concurrently.
 */
VOID fat32_mutation_lock(LITEOS_FAT32 *filesystem) {
    while (__atomic_exchange_n(&filesystem->MutationLock, 1U,
                               __ATOMIC_ACQUIRE) != 0U) {
        __asm__ volatile ("pause");
    }
}

VOID fat32_mutation_unlock(LITEOS_FAT32 *filesystem) {
    __atomic_store_n(&filesystem->MutationLock, 0U, __ATOMIC_RELEASE);
}

/*
 * 鍒犻櫎鎿嶄綔闇€瑕佸悓鏃朵慨鏀圭洰褰曢」鍜屾墍鏈?FAT 鍓湰銆傜皣閾惧揩鐓у彧淇濆瓨灏嗚
 * 娓呯┖鐨?FAT 椤癸紝澶辫触鏃跺彲浠ユ妸宸插啓鍏ョ殑椤规仮澶嶅埌鎿嶄綔鍓嶇殑鍊硷紱杩欐瘮鍙? * 鎭㈠ FAT1 鏇村畨鍏紝涔熷吋瀹瑰彲閫夌殑绗笁浠?extent FAT銆? */

BOOLEAN fat32_directory_short_name_exists(LITEOS_FAT32 *filesystem,
                                           UINT32 directory, const UINT8 name[11]) {
    UINT8 sector[4096];
    UINT32 cluster = directory;
    if (filesystem == 0 || name == 0 || !fat32_cluster_valid(filesystem, directory) ||
        filesystem->BytesPerSector > sizeof(sector)) return 0;
    for (UINT32 hops = 0U; hops < filesystem->ClusterCount; ++hops) {
        UINT64 first_sector = fat32_cluster_lba(filesystem, cluster);
        for (UINT32 sector_index = 0U; sector_index < filesystem->SectorsPerCluster;
             ++sector_index) {
            if (!fat32_read_sector(filesystem, first_sector + sector_index, sector)) return 0;
            for (UINT32 offset = 0U; offset < filesystem->BytesPerSector;
                 offset += FAT32_ENTRY_SIZE) {
                const UINT8 *entry = sector + offset;
                if (entry[0] == 0U) return 0;
                if (entry[0] == 0xE5U || entry[11] == FAT32_ATTRIBUTE_LFN) continue;
                if (fat32_short_name_equal(entry, name)) return 1;
            }
        }
        UINT32 next;
        if (!fat32_read_next_cluster(filesystem, cluster, &next) || next == 0U) return 0;
        cluster = next;
    }
    return 0;
}

UINT32 fat32_entry_cluster(const UINT8 entry[32]) {
    return ((UINT32)fat32_read_u16(entry + 20U) << 16) | fat32_read_u16(entry + 26U);
}

UINT8 fat32_short_name_checksum(const UINT8 short_name[11]) {
    UINT8 checksum = 0U;
    for (UINT32 i = 0; i < 11U; ++i) {
        checksum = (UINT8)(((checksum & 1U) != 0U ? 0x80U : 0U) +
                           (checksum >> 1) + short_name[i]);
    }
    return checksum;
}

BOOLEAN fat32_find_directory_entry_ex(LITEOS_FAT32 *filesystem, UINT32 directory_cluster,
                                       const CHAR8 *name, UINT32 name_length,
                                       UINT8 result[32], UINT64 *result_lba,
                                       UINT32 *result_offset,
                                       UINT64 lfn_lba[20], UINT32 lfn_offset[20],
                                       UINT32 *lfn_count) {
    UINT8 sector[4096];
    UINT16 lfn_characters[260];
    UINT8 short_name[11];
    BOOLEAN have_short_name = fat32_make_short_name(name, name_length, short_name);
    UINT32 cluster = directory_cluster;
    BOOLEAN lfn_valid = 0;
    UINT32 lfn_expected = 0U;
    UINT8 lfn_checksum = 0U;
    UINT32 found_lfn_count = 0U;
    if (lfn_count != 0) *lfn_count = 0U;
    fat32_lfn_reset(lfn_characters, &lfn_valid, &lfn_expected, &lfn_checksum);
    for (UINT32 chain = 0; chain < filesystem->ClusterCount; ++chain) {
        BOOLEAN fixed_root = fat32_root_directory_cluster(filesystem, cluster);
        if (!fixed_root && !fat32_cluster_valid(filesystem, cluster)) {
            return 0;
        }
        UINT64 first_sector = fixed_root ? filesystem->RootDirectoryLba :
                           fat32_cluster_lba(filesystem, cluster);
        UINT32 sector_count = fixed_root ? filesystem->RootDirectorySectors :
                            filesystem->SectorsPerCluster;
        for (UINT32 sector_index = 0; sector_index < sector_count; ++sector_index) {
            if (!fat32_read_sector(filesystem, first_sector + sector_index, sector)) {
                return 0;
            }
            for (UINT32 offset = 0; offset < filesystem->BytesPerSector; offset += FAT32_ENTRY_SIZE) {
                const UINT8 *entry = sector + offset;
                if (entry[0] == 0U) {
                    return 0;
                }
                if (entry[0] == 0xE5U) {
                    fat32_lfn_reset(lfn_characters, &lfn_valid, &lfn_expected,
                              &lfn_checksum);
                    found_lfn_count = 0U;
                    continue;
                }
                if (entry[11] == FAT32_ATTRIBUTE_LFN) {
                    if ((entry[0] & 0x40U) != 0U) found_lfn_count = 0U;
                    if (found_lfn_count < 20U) {
                        if (lfn_lba != 0) {
                            lfn_lba[found_lfn_count] = first_sector + sector_index;
                        }
                        if (lfn_offset != 0) lfn_offset[found_lfn_count] = offset;
                        ++found_lfn_count;
                    }
                    fat32_lfn_store_entry(entry, lfn_characters, &lfn_valid,
                                    &lfn_expected, &lfn_checksum);
                    continue;
                }
                BOOLEAN equal = have_short_name;
                for (UINT32 i = 0; equal && i < 11U; ++i) {
                    if (entry[i] != short_name[i]) equal = 0;
                }
                if (lfn_valid && lfn_expected == 0U &&
                    fat32_short_name_checksum(entry) == lfn_checksum &&
                    fat32_lfn_name_equal(lfn_characters, name, name_length)) {
                    equal = 1;
                }
                if (equal) {
                    for (UINT32 i = 0; i < FAT32_ENTRY_SIZE; ++i) result[i] = entry[i];
                    if (result_lba != 0) {
                        *result_lba = first_sector + sector_index;
                    }
                    if (result_offset != 0) *result_offset = offset;
                    if (lfn_count != 0 && lfn_valid && lfn_expected == 0U) {
                        *lfn_count = found_lfn_count;
                    }
                    return 1;
                }
                fat32_lfn_reset(lfn_characters, &lfn_valid, &lfn_expected,
                          &lfn_checksum);
                found_lfn_count = 0U;
            }
        }
        if (fixed_root) return 0;
        UINT32 next;
        if (!fat32_read_next_cluster(filesystem, cluster, &next) || next == 0U) {
            return 0;
        }
        cluster = next;
    }
    return 0;
}

BOOLEAN fat32_find_directory_entry(LITEOS_FAT32 *filesystem, UINT32 directory_cluster,
                                    const CHAR8 *name, UINT32 name_length,
                                    UINT8 result[32], UINT64 *result_lba,
                                    UINT32 *result_offset) {
    return fat32_find_directory_entry_ex(filesystem, directory_cluster, name, name_length,
                                   result, result_lba, result_offset, 0, 0, 0);
}

BOOLEAN fat32_resolve_path(LITEOS_FAT32 *filesystem, const CHAR8 *path,
                            UINT8 result[32], UINT64 *result_lba,
                            UINT32 *result_offset) {
    UINT32 directory = filesystem->RootCluster;
    UINT32 position = 0;
    UINT32 path_length = 0;
    if (path == 0 || path[0] == 0) return 0;
    while (path_length < LITEOS_FAT32_PATH_LENGTH && path[path_length] != 0) ++path_length;
    while (position < path_length) {
        UINT32 end = position;
        while (end < path_length && path[end] != '/') ++end;
        if (!fat32_find_directory_entry(filesystem, directory, path + position,
                                  end - position, result, result_lba,
                                  result_offset)) return 0;
        position = end;
        while (position < path_length && path[position] == '/') ++position;
        if (position == path_length) return 1;
        if ((result[11] & FAT32_DIRECTORY) == 0U) return 0;
        directory = fat32_entry_cluster(result);
    }
    return 0;
}

BOOLEAN fat32_resolve_directory_cluster(LITEOS_FAT32 *filesystem, const CHAR8 *path,
                                          UINT32 *cluster) {
    UINT32 directory;
    UINT32 position = 0U;
    UINT32 path_length = 0U;
    UINT8 entry[32];
    if (filesystem == 0 || !filesystem->Mounted || path == 0 || cluster == 0) return 0;
    while (path_length < LITEOS_FAT32_PATH_LENGTH && path[path_length] != 0) ++path_length;
    if (path_length >= LITEOS_FAT32_PATH_LENGTH) return 0;
    directory = filesystem->RootCluster;
    while (position < path_length && path[position] == '/') ++position;
    while (position < path_length) {
        UINT32 end = position;
        while (end < path_length && path[end] != '/') ++end;
        if (end == position || !fat32_find_directory_entry(filesystem, directory,
                                                      path + position, end - position,
                                                      entry, 0, 0) ||
            (entry[11] & FAT32_DIRECTORY) == 0U || !fat32_cluster_valid(filesystem,
                                                                   fat32_entry_cluster(entry))) {
            return 0;
        }
        directory = fat32_entry_cluster(entry);
        position = end;
        while (position < path_length && path[position] == '/') ++position;
    }
    *cluster = directory;
    return 1;
}


static BOOLEAN split_parent_path(const CHAR8 *path, CHAR8 parent[LITEOS_FAT32_PATH_LENGTH],
                                 CHAR8 leaf[LITEOS_FAT32_PATH_LENGTH]) {
    UINT32 length = 0U;
    UINT32 start = 0U;
    UINT32 separator = UINT32_MAX;
    UINT32 leaf_start;
    UINT32 parent_length;
    if (path == 0 || parent == 0 || leaf == 0) return 0;
    while (length < LITEOS_FAT32_PATH_LENGTH && path[length] != 0) ++length;
    if (length == 0U || length >= LITEOS_FAT32_PATH_LENGTH) return 0;
    while (start < length && path[start] == '/') ++start;
    if (start == length) return 0;
    for (UINT32 i = start; i < length; ++i) {
        if (path[i] == '/') {
            if (i == start || i + 1U == length || path[i + 1U] == '/') return 0;
            separator = i;
        }
    }
    leaf_start = separator == UINT32_MAX ? start : separator + 1U;
    parent_length = separator == UINT32_MAX ? 0U : separator - start;
    if (length - leaf_start == 0U || length - leaf_start >= LITEOS_FAT32_PATH_LENGTH ||
        parent_length >= LITEOS_FAT32_PATH_LENGTH) return 0;
    for (UINT32 i = 0; i < parent_length; ++i) parent[i] = path[start + i];
    parent[parent_length] = 0;
    for (UINT32 i = 0; i < length - leaf_start; ++i) leaf[i] = path[leaf_start + i];
    leaf[length - leaf_start] = 0;
    return 1;
}

BOOLEAN fat32_resolve_parent_directory(LITEOS_FAT32 *filesystem,
                                         const CHAR8 *path, UINT32 *directory,
                                         CHAR8 leaf[LITEOS_FAT32_PATH_LENGTH]) {
    CHAR8 parent[LITEOS_FAT32_PATH_LENGTH];
    UINT8 entry[FAT32_ENTRY_SIZE];
    if (filesystem == 0 || !filesystem->Mounted || directory == 0 || leaf == 0 ||
        !split_parent_path(path, parent, leaf)) return 0;
    *directory = filesystem->RootCluster;
    if (parent[0] == 0) return 1;
    if (!fat32_resolve_path(filesystem, parent, entry, 0, 0) ||
        (entry[11] & FAT32_DIRECTORY) == 0U ||
        (!fat32_cluster_valid(filesystem, fat32_entry_cluster(entry)) &&
         !fat32_root_directory_cluster(filesystem, fat32_entry_cluster(entry)))) return 0;
    *directory = fat32_entry_cluster(entry);
    return 1;
}

/* 鍦ㄧ洰褰曢摼涓鎵捐繛缁殑鐩綍椤癸紱闀挎枃浠跺悕闇€瑕佷竴娆′繚鐣?LFN 椤瑰拰鐭悕椤广€?*/
BOOLEAN fat32_find_directory_slots(LITEOS_FAT32 *filesystem, UINT32 directory,
                                    UINT32 slot_count, UINT64 lba[21],
                                    UINT32 offset[21], UINT32 new_clusters[21],
                                    UINT32 *new_cluster_count,
                                    UINT32 *extension_anchor) {
    UINT8 sector[4096];
    UINT32 cluster = directory;
    UINT32 run = 0U;
    UINT32 scanned = 0U;
    UINT32 extensions = 0U;
    UINT32 anchor = 0U;
    BOOLEAN after_end = 0;
    if (filesystem == 0 || slot_count == 0U || slot_count > FAT32_MAX_CREATE_SLOTS ||
        lba == 0 || offset == 0 || new_clusters == 0 || new_cluster_count == 0 ||
        extension_anchor == 0 ||
        (!fat32_cluster_valid(filesystem, directory) &&
         !fat32_root_directory_cluster(filesystem, directory)) ||
        filesystem->BytesPerSector > sizeof(sector)) {
        return 0;
    }
    *new_cluster_count = 0U;
    *extension_anchor = 0U;
    for (;;) {
        BOOLEAN fixed_root = fat32_root_directory_cluster(filesystem, cluster);
        if (scanned++ >= filesystem->ClusterCount) {
            fat32_rollback_directory_extensions(filesystem, anchor, new_clusters, extensions);
            return 0;
        }
        UINT64 first_sector = fixed_root ? filesystem->RootDirectoryLba :
                           fat32_cluster_lba(filesystem, cluster);
        UINT32 sector_count = fixed_root ? filesystem->RootDirectorySectors :
                            filesystem->SectorsPerCluster;
        for (UINT32 sector_index = 0U; sector_index < sector_count;
             ++sector_index) {
            if (!fat32_read_sector(filesystem, first_sector + sector_index, sector)) {
                fat32_rollback_directory_extensions(filesystem, anchor, new_clusters, extensions);
                return 0;
            }
            for (UINT32 entry_offset = 0U; entry_offset < filesystem->BytesPerSector;
                 entry_offset += FAT32_ENTRY_SIZE) {
                UINT8 marker = sector[entry_offset];
                BOOLEAN free = after_end || marker == 0U || marker == 0xE5U;
                if (free) {
                    if (run < slot_count) {
                        lba[run] = first_sector + sector_index;
                        offset[run] = entry_offset;
                    }
                    ++run;
                    if (marker == 0U) after_end = 1;
                    if (run == slot_count) {
                        *new_cluster_count = extensions;
                        *extension_anchor = anchor;
                        return 1;
                    }
                } else {
                    run = 0U;
                    after_end = 0;
                }
            }
        }
        if (fixed_root) {
            fat32_rollback_directory_extensions(filesystem, anchor, new_clusters, extensions);
            return 0;
        }
        UINT32 next;
        if (!fat32_read_next_cluster(filesystem, cluster, &next)) {
            fat32_rollback_directory_extensions(filesystem, anchor, new_clusters, extensions);
            return 0;
        }
        if (next != 0U) {
            cluster = next;
            continue;
        }

        if (extensions >= slot_count || !fat32_find_free_cluster(filesystem, &next) ||
            !fat32_write_file_fat_value(filesystem, cluster, next, 1U)) {
            fat32_rollback_directory_extensions(filesystem, anchor, new_clusters, extensions);
            return 0;
        }
        if (extensions == 0U) anchor = cluster;
        if (!fat32_write_file_fat_value(filesystem, next, FAT32_EOC_MIN, 1U)) {
            (void)fat32_write_file_fat_value(filesystem, cluster, FAT32_EOC_MIN, 1U);
            (void)fat32_write_file_fat_value(filesystem, next, 0U, 0U);
            fat32_rollback_directory_extensions(filesystem, anchor, new_clusters, extensions);
            return 0;
        }
        new_clusters[extensions++] = next;
        for (UINT32 sector_index = 0U; sector_index < filesystem->SectorsPerCluster;
             ++sector_index) {
            for (UINT32 byte = 0U; byte < filesystem->BytesPerSector; ++byte) sector[byte] = 0U;
            if (!fat32_write_sector(filesystem, fat32_cluster_lba(filesystem, next) + sector_index,
                              sector)) {
                fat32_rollback_directory_extensions(filesystem, anchor, new_clusters, extensions);
                return 0;
            }
        }
        cluster = next;
        after_end = 1;
    }
}

BOOLEAN fat32_write_directory_slot(LITEOS_FAT32 *filesystem, UINT64 lba,
                                     UINT32 offset, const UINT8 entry[FAT32_ENTRY_SIZE]) {
    UINT8 sector[4096];
    if (filesystem == 0 || entry == 0 || filesystem->BytesPerSector > sizeof(sector) ||
        offset > filesystem->BytesPerSector - FAT32_ENTRY_SIZE ||
        !fat32_read_sector(filesystem, lba, sector)) return 0;
    for (UINT32 i = 0U; i < FAT32_ENTRY_SIZE; ++i) sector[offset + i] = entry[i];
    return fat32_write_sector(filesystem, lba, sector);
}

BOOLEAN fat32_delete_directory_slot(LITEOS_FAT32 *filesystem, UINT64 lba,
                                     UINT32 offset) {
    UINT8 sector[4096];
    if (filesystem == 0 || filesystem->BytesPerSector > sizeof(sector) ||
        offset > filesystem->BytesPerSector - FAT32_ENTRY_SIZE ||
        !fat32_read_sector(filesystem, lba, sector)) return 0;
    sector[offset] = 0xE5U;
    return fat32_write_sector(filesystem, lba, sector);
}

BOOLEAN fat32_read_directory_slot(LITEOS_FAT32 *filesystem, UINT64 lba,
                                   UINT32 offset, UINT8 entry[FAT32_ENTRY_SIZE]) {
    UINT8 sector[4096];
    if (filesystem == 0 || entry == 0 || filesystem->BytesPerSector > sizeof(sector) ||
        offset > filesystem->BytesPerSector - FAT32_ENTRY_SIZE ||
        !fat32_read_sector(filesystem, lba, sector)) return 0;
    for (UINT32 i = 0U; i < FAT32_ENTRY_SIZE; ++i) entry[i] = sector[offset + i];
    return 1;
}

BOOLEAN fat32_allocate_zero_cluster(LITEOS_FAT32 *filesystem, UINT32 *cluster) {
    UINT8 sector[4096];
    UINT32 value;
    if (filesystem == 0 || cluster == 0 || filesystem->BytesPerSector > sizeof(sector) ||
        !fat32_find_free_cluster(filesystem, &value) ||
        !fat32_write_file_fat_value(filesystem, value, FAT32_EOC_MIN, 1U)) return 0;
    for (UINT32 byte = 0U; byte < filesystem->BytesPerSector; ++byte) sector[byte] = 0U;
    for (UINT32 sector_index = 0U; sector_index < filesystem->SectorsPerCluster;
         ++sector_index) {
        if (!fat32_write_sector(filesystem, fat32_cluster_lba(filesystem, value) + sector_index,
                          sector)) {
            (void)fat32_free_cluster_chain(filesystem, value);
            return 0;
        }
    }
    *cluster = value;
    return 1;
}

BOOLEAN fat32_initialize_directory_cluster(LITEOS_FAT32 *filesystem, UINT32 cluster,
                                             UINT32 parent) {
    UINT8 sector[4096];
    if (filesystem == 0 || !fat32_cluster_valid(filesystem, cluster) ||
        (!fat32_root_directory_cluster(filesystem, parent) &&
         !fat32_cluster_valid(filesystem, parent)) ||
        filesystem->BytesPerSector > sizeof(sector) ||
        !fat32_read_sector(filesystem, fat32_cluster_lba(filesystem, cluster), sector)) return 0;
    for (UINT32 i = 0U; i < FAT32_ENTRY_SIZE * 2U; ++i) sector[i] = 0U;
    for (UINT32 i = 0U; i < 11U; ++i) {
        sector[i] = ' ';
        sector[FAT32_ENTRY_SIZE + i] = ' ';
    }
    sector[0] = '.';
    sector[11] = FAT32_DIRECTORY;
    fat32_write_u16(sector + 20U, (UINT16)(cluster >> 16));
    fat32_write_u16(sector + 26U, (UINT16)cluster);
    sector[FAT32_ENTRY_SIZE] = '.';
    sector[FAT32_ENTRY_SIZE + 1U] = '.';
    sector[FAT32_ENTRY_SIZE + 11U] = FAT32_DIRECTORY;
    fat32_write_u16(sector + FAT32_ENTRY_SIZE + 20U, (UINT16)(parent >> 16));
    fat32_write_u16(sector + FAT32_ENTRY_SIZE + 26U, (UINT16)parent);
    return fat32_write_sector(filesystem, fat32_cluster_lba(filesystem, cluster), sector);
}

BOOLEAN fat32_directory_is_empty(LITEOS_FAT32 *filesystem, UINT32 directory) {
    UINT8 sector[4096];
    UINT32 cluster = directory;
    if (filesystem == 0 || !fat32_cluster_valid(filesystem, directory) ||
        filesystem->BytesPerSector > sizeof(sector)) return 0;
    for (UINT32 hops = 0U; hops < filesystem->ClusterCount; ++hops) {
        UINT64 first_sector = fat32_cluster_lba(filesystem, cluster);
        for (UINT32 sector_index = 0U; sector_index < filesystem->SectorsPerCluster;
             ++sector_index) {
            if (!fat32_read_sector(filesystem, first_sector + sector_index, sector)) return 0;
            for (UINT32 offset = 0U; offset < filesystem->BytesPerSector;
                 offset += FAT32_ENTRY_SIZE) {
                const UINT8 *entry = sector + offset;
                if (entry[0] == 0U) return 1;
                if (entry[0] == 0xE5U || entry[11] == FAT32_ATTRIBUTE_LFN) continue;
                BOOLEAN dot = entry[0] == '.';
                BOOLEAN dotdot = dot && entry[1] == '.';
                for (UINT32 i = 1U; i < 11U && dot; ++i) {
                    if (entry[i] != ' ') dot = 0;
                }
                if (dotdot) {
                    dotdot = 1;
                    for (UINT32 i = 2U; i < 11U; ++i) {
                        if (entry[i] != ' ') dotdot = 0;
                    }
                }
                if (!dot && !dotdot) return 0;
            }
        }
        UINT32 next;
        if (!fat32_read_next_cluster(filesystem, cluster, &next) || next == 0U) return 1;
        cluster = next;
    }
    return 0;
}

BOOLEAN fat32_file_is_open_at(LITEOS_FAT32 *filesystem, UINT64 lba,
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


