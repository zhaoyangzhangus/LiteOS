#include "fat32.h"

#ifdef LITEOS_KERNEL_BUILD
#include <kernel/kmem.h>
#else
#include <stdlib.h>
#endif

#define FAT32_EOC_MIN       0x0FFFFFF8U
#define FAT32_BAD_CLUSTER   0x0FFFFFF7U
#define FAT16_EOC_MIN       0x0000FFF8U
#define FAT16_BAD_CLUSTER   0x0000FFF7U
#define FAT_TYPE_16         16U
#define FAT_TYPE_32         32U
#define FAT32_DIRECTORY     0x10U
#define FAT32_ENTRY_SIZE    32U
#define FAT32_ATTRIBUTE_LFN 0x0FU
#define FAT32_MAX_LFN_ENTRIES 20U
#define FAT32_MAX_CREATE_SLOTS (FAT32_MAX_LFN_ENTRIES + 1U)

static const LITEOS_VFS_FILE_OPERATIONS g_fat32_operations;

/*
 * 删除操作需要同时修改目录项和所有 FAT 副本。簇链快照只保存将被
 * 清空的 FAT 项，失败时可以把已写入的项恢复到操作前的值；这比只
 * 恢复 FAT1 更安全，也兼容可选的第三份 extent FAT。
 */
typedef struct fat_chain_snapshot {
    UINT32 *Clusters;
    UINT32 *Values;
    UINT32 Count;
    UINT32 Capacity;
    UINT32 FatCount;
} FAT_CHAIN_SNAPSHOT;

static VOID *fat_transaction_alloc(size_t size) {
#ifdef LITEOS_KERNEL_BUILD
    return kmalloc(size, 0);
#else
    return malloc(size);
#endif
}

static VOID fat_transaction_free(VOID *memory) {
#ifdef LITEOS_KERNEL_BUILD
    kfree(memory);
#else
    free(memory);
#endif
}

static UINT16 read_u16(const UINT8 *data) {
    return (UINT16)data[0] | ((UINT16)data[1] << 8);
}

static UINT32 read_u32(const UINT8 *data) {
    return (UINT32)data[0] | ((UINT32)data[1] << 8) |
           ((UINT32)data[2] << 16) | ((UINT32)data[3] << 24);
}

static void write_u16(UINT8 *data, UINT16 value) {
    data[0] = (UINT8)value;
    data[1] = (UINT8)(value >> 8);
}

static void write_u32(UINT8 *data, UINT32 value) {
    data[0] = (UINT8)value;
    data[1] = (UINT8)(value >> 8);
    data[2] = (UINT8)(value >> 16);
    data[3] = (UINT8)(value >> 24);
}

static BOOLEAN is_power_of_two(UINT32 value) {
    return value != 0U && (value & (value - 1U)) == 0U;
}

static BOOLEAN cluster_valid(const LITEOS_FAT32 *filesystem, UINT32 cluster) {
    return filesystem != 0 && cluster >= 2U && cluster <= filesystem->ClusterCount + 1U;
}

static BOOLEAN root_directory_cluster(const LITEOS_FAT32 *filesystem,
                                      UINT32 cluster) {
    return filesystem != 0 && filesystem->FatType == FAT_TYPE_16 && cluster == 0U;
}

static BOOLEAN read_sector(const LITEOS_FAT32 *filesystem, UINT64 lba, UINT8 *buffer) {
    if (filesystem == 0 || buffer == 0 || filesystem->Device == 0 ||
        lba >= filesystem->Device->BlockCount) return 0;
    if (filesystem->Cache.Initialized) {
        return liteos_block_cache_read((LITEOS_BLOCK_CACHE *)&filesystem->Cache, lba, buffer);
    }
    return liteos_block_read(filesystem->Device, lba, 1U, buffer);
}

static BOOLEAN write_sector(LITEOS_FAT32 *filesystem, UINT64 lba,
                            const UINT8 *buffer) {
    if (filesystem == 0 || buffer == 0 || filesystem->Device == 0 ||
        lba >= filesystem->Device->BlockCount) return 0;
    if (filesystem->Cache.Initialized) {
        return liteos_block_cache_write(&filesystem->Cache, lba, buffer);
    }
    return liteos_block_write(filesystem->Device, lba, 1U, buffer);
}

static UINT64 cluster_lba(const LITEOS_FAT32 *filesystem, UINT32 cluster) {
    return filesystem->DataStartLba +
           (UINT64)(cluster - 2U) * filesystem->SectorsPerCluster;
}

static BOOLEAN read_fat_entry(const LITEOS_FAT32 *filesystem, UINT32 fat_number,
                              UINT32 cluster, UINT32 *value) {
    UINT8 sector[4096];
    UINT64 byte_offset;
    UINT64 lba;
    UINT32 offset;
    UINT32 entry_size;
    UINT64 fat_start;
    if (filesystem == 0 || value == 0 || !cluster_valid(filesystem, cluster) ||
        fat_number == 0U || fat_number > filesystem->FatCount) return 0;
    entry_size = filesystem->FatType == FAT_TYPE_16 ? sizeof(UINT16) : sizeof(UINT32);
    byte_offset = (UINT64)cluster * entry_size;
    lba = byte_offset / filesystem->BytesPerSector;
    offset = (UINT32)(byte_offset % filesystem->BytesPerSector);
    if (filesystem->BytesPerSector > sizeof(sector) ||
        offset + entry_size > filesystem->BytesPerSector) return 0;
    fat_start = filesystem->FatStartLba + (UINT64)(fat_number - 1U) * filesystem->FatSectors;
    if (!read_sector(filesystem, fat_start + lba, sector)) return 0;
    *value = filesystem->FatType == FAT_TYPE_16 ? read_u16(sector + offset) :
             read_u32(sector + offset) & 0x0FFFFFFFU;
    return 1;
}

static BOOLEAN write_fat_entry(LITEOS_FAT32 *filesystem, UINT32 fat_number,
                               UINT32 cluster, UINT32 value) {
    UINT8 sector[4096];
    UINT64 byte_offset;
    UINT64 lba;
    UINT32 offset;
    UINT32 entry_size;
    UINT64 fat_start;
    if (filesystem == 0 || !cluster_valid(filesystem, cluster) ||
        fat_number == 0U || fat_number > filesystem->FatCount ||
        filesystem->BytesPerSector > sizeof(sector)) return 0;
    entry_size = filesystem->FatType == FAT_TYPE_16 ? sizeof(UINT16) : sizeof(UINT32);
    byte_offset = (UINT64)cluster * entry_size;
    lba = byte_offset / filesystem->BytesPerSector;
    offset = (UINT32)(byte_offset % filesystem->BytesPerSector);
    if (offset + entry_size > filesystem->BytesPerSector) return 0;
    fat_start = filesystem->FatStartLba +
                (UINT64)(fat_number - 1U) * filesystem->FatSectors;
    if (!read_sector(filesystem, fat_start + lba, sector)) return 0;
    if (filesystem->FatType == FAT_TYPE_16) write_u16(sector + offset, (UINT16)value);
    else write_u32(sector + offset, value & 0x0FFFFFFFU);
    return write_sector(filesystem, fat_start + lba, sector);
}

/* FAT3 是可选 extent 表，不保存普通的簇链值。 */
static BOOLEAN write_file_fat_value(LITEOS_FAT32 *filesystem, UINT32 cluster,
                                    UINT32 chain_value, UINT32 extent_value) {
    for (UINT32 fat = 1U; fat <= filesystem->FatCount; ++fat) {
        UINT32 value = filesystem->Fat3Available && fat == 3U ?
                       extent_value : chain_value;
        if (!write_fat_entry(filesystem, fat, cluster, value)) return 0;
    }
    return 1;
}

static BOOLEAN find_free_cluster(LITEOS_FAT32 *filesystem, UINT32 *cluster) {
    UINT32 value;
    if (filesystem == 0 || cluster == 0) return 0;
    for (UINT32 candidate = 2U; candidate <= filesystem->ClusterCount + 1U;
         ++candidate) {
        if (!read_fat_entry(filesystem, 1U, candidate, &value)) return 0;
        if (value == 0U) {
            *cluster = candidate;
            return 1;
        }
    }
    return 0;
}

static BOOLEAN read_next_cluster(const LITEOS_FAT32 *filesystem, UINT32 cluster,
                                 UINT32 *next) {
    UINT32 value;
    UINT32 eoc_min = filesystem != 0 && filesystem->FatType == FAT_TYPE_16 ?
                     FAT16_EOC_MIN : FAT32_EOC_MIN;
    UINT32 bad_cluster = filesystem != 0 && filesystem->FatType == FAT_TYPE_16 ?
                         FAT16_BAD_CLUSTER : FAT32_BAD_CLUSTER;
    if (!read_fat_entry(filesystem, 1U, cluster, &value) || value == bad_cluster) return 0;
    if (value >= eoc_min) {
        *next = 0;
        return 1;
    }
    if (!cluster_valid(filesystem, value)) return 0;
    *next = value;
    return 1;
}

static UINT32 extent_length(const LITEOS_FAT32 *filesystem, UINT32 cluster) {
    UINT32 length;
    if (!filesystem->Fat3Available || !read_fat_entry(filesystem, 3U, cluster, &length) ||
        length == 0U || length > filesystem->ClusterCount + 2U - cluster) return 1U;
    return length;
}

static BOOLEAN make_short_name(const CHAR8 *text, UINT32 length, UINT8 result[11]) {
    UINT32 name_length = 0;
    UINT32 extension_length = 0;
    UINT32 dot = length;
    for (UINT32 i = 0; i < length; ++i) {
        if (text[i] == '.') {
            if (dot != length) return 0;
            dot = i;
        }
        if (text[i] == '/') return 0;
    }
    name_length = dot;
    if (dot < length) extension_length = length - dot - 1U;
    if (name_length == 0U || name_length > 8U || extension_length > 3U ||
        (dot < length && extension_length == 0U)) return 0;
    for (UINT32 i = 0; i < 11U; ++i) result[i] = ' ';
    for (UINT32 i = 0; i < name_length; ++i) {
        CHAR8 character = text[i];
        if (character >= 'a' && character <= 'z') character = (CHAR8)(character - 'a' + 'A');
        if (character == ' ' || character == '+' || character == ',' || character == ';' ||
            character == '=' || character == '[' || character == ']') return 0;
        result[i] = (UINT8)character;
    }
    for (UINT32 i = 0; i < extension_length; ++i) {
        CHAR8 character = text[dot + 1U + i];
        if (character >= 'a' && character <= 'z') character = (CHAR8)(character - 'a' + 'A');
        result[8U + i] = (UINT8)character;
    }
    return 1;
}

static BOOLEAN short_name_equal(const UINT8 left[11], const UINT8 right[11]) {
    for (UINT32 i = 0U; i < 11U; ++i) {
        if (left[i] != right[i]) return 0;
    }
    return 1;
}

/* FAT 短名别名只保留可移植字符，长名本身仍由 LFN 项保存。 */
static BOOLEAN short_alias_character(CHAR8 character) {
    if ((character >= 'A' && character <= 'Z') ||
        (character >= 'a' && character <= 'z') ||
        (character >= '0' && character <= '9')) return 1;
    return character == '$' || character == '%' || character == 0x27 ||
           character == '-' || character == '_' || character == '@' ||
           character == '~' || character == '`' || character == '!' ||
           character == '(' || character == ')' || character == '^' ||
           character == '#' || character == '&';
}

static UINT32 decimal_digits(UINT32 value) {
    UINT32 digits = 1U;
    while (value >= 10U) {
        value /= 10U;
        ++digits;
    }
    return digits;
}

static BOOLEAN make_long_name_alias(const CHAR8 *name, UINT32 length,
                                    UINT32 suffix, UINT8 result[11]) {
    UINT32 dot = length;
    UINT32 base_count = 0U;
    UINT32 extension_count = 0U;
    CHAR8 base[8] = {0};
    CHAR8 extension[3] = {0};
    UINT32 suffix_digits;
    UINT32 base_capacity;
    if (name == 0 || result == 0 || length == 0U || length > 255U || suffix == 0U) return 0;
    for (UINT32 i = 0U; i < length; ++i) {
        if (name[i] == '.') dot = i;
    }
    for (UINT32 i = 0U; i < dot; ++i) {
        CHAR8 character = name[i];
        if (character == ' ' || character == '.') continue;
        if (!short_alias_character(character)) continue;
        if (base_count < sizeof(base)) {
            if (character >= 'a' && character <= 'z') character = (CHAR8)(character - 'a' + 'A');
            base[base_count++] = character;
        }
    }
    if (dot < length) {
        for (UINT32 i = dot + 1U; i < length && extension_count < sizeof(extension); ++i) {
            CHAR8 character = name[i];
            if (!short_alias_character(character)) continue;
            if (character >= 'a' && character <= 'z') character = (CHAR8)(character - 'a' + 'A');
            extension[extension_count++] = character;
        }
    }
    suffix_digits = decimal_digits(suffix);
    if (base_count == 0U || suffix_digits + 1U >= 8U) return 0;
    base_capacity = 8U - suffix_digits - 1U;
    for (UINT32 i = 0U; i < 11U; ++i) result[i] = ' ';
    for (UINT32 i = 0U; i < base_count && i < base_capacity; ++i) result[i] = (UINT8)base[i];
    result[base_capacity] = '~';
    UINT32 divisor = 1U;
    for (UINT32 i = 1U; i < suffix_digits; ++i) divisor *= 10U;
    for (UINT32 i = 0U; i < suffix_digits; ++i) {
        result[base_capacity + 1U + i] = (UINT8)('0' + (suffix / divisor) % 10U);
        divisor /= 10U;
    }
    for (UINT32 i = 0U; i < extension_count; ++i) result[8U + i] = (UINT8)extension[i];
    return 1;
}

static BOOLEAN directory_short_name_exists(LITEOS_FAT32 *filesystem,
                                           UINT32 directory, const UINT8 name[11]) {
    UINT8 sector[4096];
    UINT32 cluster = directory;
    if (filesystem == 0 || name == 0 || !cluster_valid(filesystem, directory) ||
        filesystem->BytesPerSector > sizeof(sector)) return 0;
    for (UINT32 hops = 0U; hops < filesystem->ClusterCount; ++hops) {
        UINT64 first_sector = cluster_lba(filesystem, cluster);
        for (UINT32 sector_index = 0U; sector_index < filesystem->SectorsPerCluster;
             ++sector_index) {
            if (!read_sector(filesystem, first_sector + sector_index, sector)) return 0;
            for (UINT32 offset = 0U; offset < filesystem->BytesPerSector;
                 offset += FAT32_ENTRY_SIZE) {
                const UINT8 *entry = sector + offset;
                if (entry[0] == 0U) return 0;
                if (entry[0] == 0xE5U || entry[11] == FAT32_ATTRIBUTE_LFN) continue;
                if (short_name_equal(entry, name)) return 1;
            }
        }
        UINT32 next;
        if (!read_next_cluster(filesystem, cluster, &next) || next == 0U) return 0;
        cluster = next;
    }
    return 0;
}

static UINT32 entry_cluster(const UINT8 entry[32]) {
    return ((UINT32)read_u16(entry + 20U) << 16) | read_u16(entry + 26U);
}

static UINT8 short_name_checksum(const UINT8 short_name[11]) {
    UINT8 checksum = 0U;
    for (UINT32 i = 0; i < 11U; ++i) {
        checksum = (UINT8)(((checksum & 1U) != 0U ? 0x80U : 0U) +
                           (checksum >> 1) + short_name[i]);
    }
    return checksum;
}

static void lfn_reset(UINT16 characters[260], BOOLEAN *valid,
                      UINT32 *expected, UINT8 *checksum) {
    for (UINT32 i = 0; i < 260U; ++i) characters[i] = 0xFFFFU;
    *valid = 0;
    *expected = 0U;
    *checksum = 0U;
}

static void lfn_store_entry(const UINT8 entry[32], UINT16 characters[260],
                            BOOLEAN *valid, UINT32 *expected, UINT8 *checksum) {
    UINT32 ordinal = entry[0] & 0x1FU;
    static const UINT8 offsets[13] = {
        1U, 3U, 5U, 7U, 9U, 14U, 16U, 18U, 20U, 22U, 24U, 28U, 30U
    };
    if (ordinal == 0U || ordinal > 20U ||
        ((entry[0] & 0x40U) != 0U && ordinal == 0U)) {
        *valid = 0;
        return;
    }
    if ((entry[0] & 0x40U) != 0U) {
        lfn_reset(characters, valid, expected, checksum);
        *valid = 1;
        *expected = ordinal;
        *checksum = entry[13];
    }
    if (!*valid || ordinal != *expected || entry[13] != *checksum) {
        *valid = 0;
        return;
    }
    UINT32 base = (ordinal - 1U) * 13U;
    for (UINT32 i = 0; i < 13U; ++i) {
        characters[base + i] = read_u16(entry + offsets[i]);
    }
    --*expected;
}

static BOOLEAN lfn_name_equal(const UINT16 characters[260], const CHAR8 *name,
                              UINT32 length) {
    if (name == 0 || length == 0U || length > 255U) return 0;
    for (UINT32 i = 0; i < length; ++i) {
        CHAR8 character = name[i];
        UINT16 stored = characters[i];
        if (character >= 'a' && character <= 'z') {
            character = (CHAR8)(character - 'a' + 'A');
        }
        if (stored >= 'a' && stored <= 'z') {
            stored = (UINT16)(stored - 'a' + 'A');
        }
        if (stored != (UINT16)(UINT8)character) return 0;
    }
    return characters[length] == 0U || characters[length] == 0xFFFFU;
}

static BOOLEAN find_directory_entry_ex(LITEOS_FAT32 *filesystem, UINT32 directory_cluster,
                                       const CHAR8 *name, UINT32 name_length,
                                       UINT8 result[32], UINT64 *result_lba,
                                       UINT32 *result_offset,
                                       UINT64 lfn_lba[20], UINT32 lfn_offset[20],
                                       UINT32 *lfn_count) {
    UINT8 sector[4096];
    UINT16 lfn_characters[260];
    UINT8 short_name[11];
    BOOLEAN have_short_name = make_short_name(name, name_length, short_name);
    UINT32 cluster = directory_cluster;
    BOOLEAN lfn_valid = 0;
    UINT32 lfn_expected = 0U;
    UINT8 lfn_checksum = 0U;
    UINT32 found_lfn_count = 0U;
    if (lfn_count != 0) *lfn_count = 0U;
    lfn_reset(lfn_characters, &lfn_valid, &lfn_expected, &lfn_checksum);
    for (UINT32 chain = 0; chain < filesystem->ClusterCount; ++chain) {
        BOOLEAN fixed_root = root_directory_cluster(filesystem, cluster);
        if (!fixed_root && !cluster_valid(filesystem, cluster)) {
            return 0;
        }
        UINT64 first_sector = fixed_root ? filesystem->RootDirectoryLba :
                           cluster_lba(filesystem, cluster);
        UINT32 sector_count = fixed_root ? filesystem->RootDirectorySectors :
                            filesystem->SectorsPerCluster;
        for (UINT32 sector_index = 0; sector_index < sector_count; ++sector_index) {
            if (!read_sector(filesystem, first_sector + sector_index, sector)) {
                return 0;
            }
            for (UINT32 offset = 0; offset < filesystem->BytesPerSector; offset += FAT32_ENTRY_SIZE) {
                const UINT8 *entry = sector + offset;
                if (entry[0] == 0U) {
                    return 0;
                }
                if (entry[0] == 0xE5U) {
                    lfn_reset(lfn_characters, &lfn_valid, &lfn_expected,
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
                    lfn_store_entry(entry, lfn_characters, &lfn_valid,
                                    &lfn_expected, &lfn_checksum);
                    continue;
                }
                BOOLEAN equal = have_short_name;
                for (UINT32 i = 0; equal && i < 11U; ++i) {
                    if (entry[i] != short_name[i]) equal = 0;
                }
                if (lfn_valid && lfn_expected == 0U &&
                    short_name_checksum(entry) == lfn_checksum &&
                    lfn_name_equal(lfn_characters, name, name_length)) {
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
                lfn_reset(lfn_characters, &lfn_valid, &lfn_expected,
                          &lfn_checksum);
                found_lfn_count = 0U;
            }
        }
        if (fixed_root) return 0;
        UINT32 next;
        if (!read_next_cluster(filesystem, cluster, &next) || next == 0U) {
            return 0;
        }
        cluster = next;
    }
    return 0;
}

static BOOLEAN find_directory_entry(LITEOS_FAT32 *filesystem, UINT32 directory_cluster,
                                    const CHAR8 *name, UINT32 name_length,
                                    UINT8 result[32], UINT64 *result_lba,
                                    UINT32 *result_offset) {
    return find_directory_entry_ex(filesystem, directory_cluster, name, name_length,
                                   result, result_lba, result_offset, 0, 0, 0);
}

static BOOLEAN resolve_path(LITEOS_FAT32 *filesystem, const CHAR8 *path,
                            UINT8 result[32], UINT64 *result_lba,
                            UINT32 *result_offset) {
    UINT32 directory = filesystem->RootCluster;
    UINT32 position = 0;
    UINT32 path_length = 0;
    if (path == 0 || path[0] == 0) return 0;
    while (path_length < LITEOS_VFS_PATH_LENGTH && path[path_length] != 0) ++path_length;
    while (position < path_length) {
        UINT32 end = position;
        while (end < path_length && path[end] != '/') ++end;
        if (!find_directory_entry(filesystem, directory, path + position,
                                  end - position, result, result_lba,
                                  result_offset)) return 0;
        position = end;
        while (position < path_length && path[position] == '/') ++position;
        if (position == path_length) return 1;
        if ((result[11] & FAT32_DIRECTORY) == 0U) return 0;
        directory = entry_cluster(result);
    }
    return 0;
}

static BOOLEAN resolve_directory_cluster(LITEOS_FAT32 *filesystem, const CHAR8 *path,
                                          UINT32 *cluster) {
    UINT32 directory;
    UINT32 position = 0U;
    UINT32 path_length = 0U;
    UINT8 entry[32];
    if (filesystem == 0 || !filesystem->Mounted || path == 0 || cluster == 0) return 0;
    while (path_length < LITEOS_VFS_PATH_LENGTH && path[path_length] != 0) ++path_length;
    if (path_length >= LITEOS_VFS_PATH_LENGTH) return 0;
    directory = filesystem->RootCluster;
    while (position < path_length && path[position] == '/') ++position;
    while (position < path_length) {
        UINT32 end = position;
        while (end < path_length && path[end] != '/') ++end;
        if (end == position || !find_directory_entry(filesystem, directory,
                                                      path + position, end - position,
                                                      entry, 0, 0) ||
            (entry[11] & FAT32_DIRECTORY) == 0U || !cluster_valid(filesystem,
                                                                   entry_cluster(entry))) {
            return 0;
        }
        directory = entry_cluster(entry);
        position = end;
        while (position < path_length && path[position] == '/') ++position;
    }
    *cluster = directory;
    return 1;
}

static UINT32 fat32_short_name_text(const UINT8 entry[32], CHAR8 output[OS_FILE_NAME_MAX]) {
    UINT32 position = 0U;
    UINT32 base_length = 8U;
    UINT32 extension_length = 3U;
    while (base_length != 0U && entry[base_length - 1U] == ' ') --base_length;
    while (extension_length != 0U && entry[8U + extension_length - 1U] == ' ') {
        --extension_length;
    }
    while (position < base_length && position + 1U < OS_FILE_NAME_MAX) {
        output[position] = (CHAR8)entry[position];
        ++position;
    }
    if (extension_length != 0U && position + 1U < OS_FILE_NAME_MAX) {
        output[position++] = '.';
        for (UINT32 index = 0U; index < extension_length &&
             position + 1U < OS_FILE_NAME_MAX; ++index) {
            output[position++] = (CHAR8)entry[8U + index];
        }
    }
    output[position] = 0;
    return position;
}

static UINT32 fat32_lfn_name_text(const UINT16 characters[260],
                                  CHAR8 output[OS_FILE_NAME_MAX]) {
    UINT32 position = 0U;
    while (position < 259U && characters[position] != 0U &&
           characters[position] != 0xFFFFU) {
        if (position + 1U < OS_FILE_NAME_MAX) {
            output[position] = characters[position] <= 0x7FU ?
                (CHAR8)characters[position] : '?';
        }
        ++position;
    }
    if (position >= OS_FILE_NAME_MAX) position = OS_FILE_NAME_MAX - 1U;
    output[position] = 0;
    return position;
}

static BOOLEAN fat32_dot_entry(const UINT8 entry[32]) {
    if (entry[0] != '.') return 0;
    if (entry[1] == '.') {
        for (UINT32 index = 2U; index < 11U; ++index) {
            if (entry[index] != ' ') return 0;
        }
        return 1;
    }
    for (UINT32 index = 1U; index < 11U; ++index) {
        if (entry[index] != ' ') return 0;
    }
    return 1;
}

static BOOLEAN locate_file_cluster(LITEOS_FAT32_FILE *file, UINT32 logical_cluster,
                                   UINT32 *physical_cluster) {
    LITEOS_FAT32 *filesystem = file->FileSystem;
    UINT32 start;
    UINT32 logical = 0;
    if (file->FirstCluster == 0U) return 0;
    if (file->CursorValid && logical_cluster >= file->CursorLogicalStart &&
        logical_cluster - file->CursorLogicalStart < file->CursorLength) {
        *physical_cluster = file->CursorPhysicalStart +
                            logical_cluster - file->CursorLogicalStart;
        return cluster_valid(filesystem, *physical_cluster);
    }
    start = file->FirstCluster;
    for (UINT32 hops = 0; hops < filesystem->ClusterCount; ++hops) {
        UINT32 length = extent_length(filesystem, start);
        if (logical_cluster - logical < length && logical_cluster >= logical) {
            file->CursorValid = 1;
            file->CursorLogicalStart = logical;
            file->CursorPhysicalStart = start;
            file->CursorLength = length;
            *physical_cluster = start + logical_cluster - logical;
            return cluster_valid(filesystem, *physical_cluster);
        }
        UINT32 next;
        if (!read_next_cluster(filesystem, start + length - 1U, &next) || next == 0U) return 0;
        logical += length;
        start = next;
    }
    return 0;
}

static BOOLEAN count_file_clusters(const LITEOS_FAT32 *filesystem, UINT32 first,
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
        if (!cluster_valid(filesystem, cluster) ||
            (length = extent_length(filesystem, cluster)) == 0U ||
            total > UINT64_MAX - length) return 0;
        total += length;
        if (last != 0) *last = cluster + length - 1U;
        if (!read_next_cluster(filesystem, cluster + length - 1U, &next)) return 0;
        if (next == 0U) {
            *count = total;
            return 1;
        }
        cluster = next;
    }
    return 0;
}

static VOID fat_snapshot_destroy(FAT_CHAIN_SNAPSHOT *snapshot) {
    if (snapshot == 0) return;
    fat_transaction_free(snapshot->Clusters);
    fat_transaction_free(snapshot->Values);
    snapshot->Clusters = 0;
    snapshot->Values = 0;
    snapshot->Count = 0U;
    snapshot->Capacity = 0U;
    snapshot->FatCount = 0U;
}

static BOOLEAN fat_snapshot_reserve(FAT_CHAIN_SNAPSHOT *snapshot) {
    UINT32 capacity;
    UINT64 value_count;
    UINT32 *clusters;
    UINT32 *values;
    if (snapshot == 0 || snapshot->FatCount == 0U) return 0;
    if (snapshot->Count < snapshot->Capacity) return 1;
    capacity = snapshot->Capacity == 0U ? 16U : snapshot->Capacity * 2U;
    value_count = (UINT64)capacity * snapshot->FatCount;
    if (capacity < snapshot->Capacity ||
        value_count > (UINT64)((size_t)-1 / sizeof(UINT32))) {
        return 0;
    }
    clusters = (UINT32 *)fat_transaction_alloc((size_t)capacity * sizeof(UINT32));
    values = (UINT32 *)fat_transaction_alloc((size_t)value_count * sizeof(UINT32));
    if (clusters == 0 || values == 0) {
        fat_transaction_free(clusters);
        fat_transaction_free(values);
        return 0;
    }
    for (UINT32 i = 0U; i < snapshot->Count; ++i) {
        clusters[i] = snapshot->Clusters[i];
        for (UINT32 fat = 0U; fat < snapshot->FatCount; ++fat) {
            values[(UINT64)i * snapshot->FatCount + fat] =
                snapshot->Values[(UINT64)i * snapshot->FatCount + fat];
        }
    }
    fat_transaction_free(snapshot->Clusters);
    fat_transaction_free(snapshot->Values);
    snapshot->Clusters = clusters;
    snapshot->Values = values;
    snapshot->Capacity = capacity;
    return 1;
}

static BOOLEAN fat_snapshot_chain(const LITEOS_FAT32 *filesystem, UINT32 first,
                                  FAT_CHAIN_SNAPSHOT *snapshot) {
    UINT32 cluster = first;
    if (filesystem == 0 || snapshot == 0 || first == 0U ||
        !cluster_valid(filesystem, first)) return first == 0U;
    snapshot->FatCount = filesystem->FatCount;
    for (UINT32 hops = 0U; hops < filesystem->ClusterCount; ++hops) {
        UINT32 next;
        if (!cluster_valid(filesystem, cluster) || !fat_snapshot_reserve(snapshot)) {
            return 0;
        }
        snapshot->Clusters[snapshot->Count] = cluster;
        for (UINT32 fat = 1U; fat <= filesystem->FatCount; ++fat) {
            if (!read_fat_entry(filesystem, fat, cluster,
                                &snapshot->Values[(UINT64)snapshot->Count *
                                                  snapshot->FatCount + fat - 1U])) {
                return 0;
            }
        }
        ++snapshot->Count;
        if (!read_next_cluster(filesystem, cluster, &next)) return 0;
        if (next == 0U) return 1;
        cluster = next;
    }
    return 0;
}

static VOID fat_snapshot_restore(LITEOS_FAT32 *filesystem,
                                 const FAT_CHAIN_SNAPSHOT *snapshot,
                                 UINT32 count) {
    if (filesystem == 0 || snapshot == 0 || snapshot->Values == 0) return;
    if (count > snapshot->Count) count = snapshot->Count;
    /* 逆序恢复，尽量先恢复链尾，避免故障中断时留下更短的可见链。 */
    while (count != 0U) {
        UINT32 index = --count;
        for (UINT32 fat = 1U; fat <= snapshot->FatCount; ++fat) {
            (void)write_fat_entry(filesystem, fat, snapshot->Clusters[index],
                                  snapshot->Values[(UINT64)index * snapshot->FatCount +
                                                   fat - 1U]);
        }
    }
}

static BOOLEAN free_cluster_chain(LITEOS_FAT32 *filesystem, UINT32 first) {
    FAT_CHAIN_SNAPSHOT snapshot = {0};
    if (first == 0U) return 1;
    if (!fat_snapshot_chain(filesystem, first, &snapshot)) {
        fat_snapshot_destroy(&snapshot);
        return 0;
    }
    for (UINT32 index = 0U; index < snapshot.Count; ++index) {
        if (!write_file_fat_value(filesystem, snapshot.Clusters[index], 0U, 0U)) {
            fat_snapshot_restore(filesystem, &snapshot, index + 1U);
            fat_snapshot_destroy(&snapshot);
            return 0;
        }
    }
    fat_snapshot_destroy(&snapshot);
    return 1;
}

/* 将路径拆成父目录和最后一个 8.3 名称，拒绝空路径及空路径分量。 */
static BOOLEAN split_parent_path(const CHAR8 *path, CHAR8 parent[LITEOS_VFS_PATH_LENGTH],
                                 CHAR8 leaf[LITEOS_VFS_PATH_LENGTH]) {
    UINT32 length = 0U;
    UINT32 start = 0U;
    UINT32 separator = UINT32_MAX;
    UINT32 leaf_start;
    UINT32 parent_length;
    if (path == 0 || parent == 0 || leaf == 0) return 0;
    while (length < LITEOS_VFS_PATH_LENGTH && path[length] != 0) ++length;
    if (length == 0U || length >= LITEOS_VFS_PATH_LENGTH) return 0;
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
    if (length - leaf_start == 0U || length - leaf_start >= LITEOS_VFS_PATH_LENGTH ||
        parent_length >= LITEOS_VFS_PATH_LENGTH) return 0;
    for (UINT32 i = 0; i < parent_length; ++i) parent[i] = path[start + i];
    parent[parent_length] = 0;
    for (UINT32 i = 0; i < length - leaf_start; ++i) leaf[i] = path[leaf_start + i];
    leaf[length - leaf_start] = 0;
    return 1;
}

static BOOLEAN resolve_parent_directory(LITEOS_FAT32 *filesystem,
                                         const CHAR8 *path, UINT32 *directory,
                                         CHAR8 leaf[LITEOS_VFS_PATH_LENGTH]) {
    CHAR8 parent[LITEOS_VFS_PATH_LENGTH];
    UINT8 entry[FAT32_ENTRY_SIZE];
    if (filesystem == 0 || !filesystem->Mounted || directory == 0 || leaf == 0 ||
        !split_parent_path(path, parent, leaf)) return 0;
    *directory = filesystem->RootCluster;
    if (parent[0] == 0) return 1;
    if (!resolve_path(filesystem, parent, entry, 0, 0) ||
        (entry[11] & FAT32_DIRECTORY) == 0U ||
        (!cluster_valid(filesystem, entry_cluster(entry)) &&
         !root_directory_cluster(filesystem, entry_cluster(entry)))) return 0;
    *directory = entry_cluster(entry);
    return 1;
}

static void rollback_directory_extensions(LITEOS_FAT32 *filesystem, UINT32 anchor,
                                           const UINT32 clusters[21], UINT32 count) {
    if (filesystem == 0 || count == 0U) return;
    (void)write_file_fat_value(filesystem, anchor, FAT32_EOC_MIN, 1U);
    (void)free_cluster_chain(filesystem, clusters[0]);
}

/* 在目录链中寻找连续的目录项；长文件名需要一次保留 LFN 项和短名项。 */
static BOOLEAN find_directory_slots(LITEOS_FAT32 *filesystem, UINT32 directory,
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
        (!cluster_valid(filesystem, directory) &&
         !root_directory_cluster(filesystem, directory)) ||
        filesystem->BytesPerSector > sizeof(sector)) {
        return 0;
    }
    *new_cluster_count = 0U;
    *extension_anchor = 0U;
    for (;;) {
        BOOLEAN fixed_root = root_directory_cluster(filesystem, cluster);
        if (scanned++ >= filesystem->ClusterCount) {
            rollback_directory_extensions(filesystem, anchor, new_clusters, extensions);
            return 0;
        }
        UINT64 first_sector = fixed_root ? filesystem->RootDirectoryLba :
                           cluster_lba(filesystem, cluster);
        UINT32 sector_count = fixed_root ? filesystem->RootDirectorySectors :
                            filesystem->SectorsPerCluster;
        for (UINT32 sector_index = 0U; sector_index < sector_count;
             ++sector_index) {
            if (!read_sector(filesystem, first_sector + sector_index, sector)) {
                rollback_directory_extensions(filesystem, anchor, new_clusters, extensions);
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
            rollback_directory_extensions(filesystem, anchor, new_clusters, extensions);
            return 0;
        }
        UINT32 next;
        if (!read_next_cluster(filesystem, cluster, &next)) {
            rollback_directory_extensions(filesystem, anchor, new_clusters, extensions);
            return 0;
        }
        if (next != 0U) {
            cluster = next;
            continue;
        }

        if (extensions >= slot_count || !find_free_cluster(filesystem, &next) ||
            !write_file_fat_value(filesystem, cluster, next, 1U)) {
            rollback_directory_extensions(filesystem, anchor, new_clusters, extensions);
            return 0;
        }
        if (extensions == 0U) anchor = cluster;
        if (!write_file_fat_value(filesystem, next, FAT32_EOC_MIN, 1U)) {
            (void)write_file_fat_value(filesystem, cluster, FAT32_EOC_MIN, 1U);
            (void)write_file_fat_value(filesystem, next, 0U, 0U);
            rollback_directory_extensions(filesystem, anchor, new_clusters, extensions);
            return 0;
        }
        new_clusters[extensions++] = next;
        for (UINT32 sector_index = 0U; sector_index < filesystem->SectorsPerCluster;
             ++sector_index) {
            for (UINT32 byte = 0U; byte < filesystem->BytesPerSector; ++byte) sector[byte] = 0U;
            if (!write_sector(filesystem, cluster_lba(filesystem, next) + sector_index,
                              sector)) {
                rollback_directory_extensions(filesystem, anchor, new_clusters, extensions);
                return 0;
            }
        }
        cluster = next;
        after_end = 1;
    }
}

static BOOLEAN write_directory_slot(LITEOS_FAT32 *filesystem, UINT64 lba,
                                     UINT32 offset, const UINT8 entry[FAT32_ENTRY_SIZE]) {
    UINT8 sector[4096];
    if (filesystem == 0 || entry == 0 || filesystem->BytesPerSector > sizeof(sector) ||
        offset > filesystem->BytesPerSector - FAT32_ENTRY_SIZE ||
        !read_sector(filesystem, lba, sector)) return 0;
    for (UINT32 i = 0U; i < FAT32_ENTRY_SIZE; ++i) sector[offset + i] = entry[i];
    return write_sector(filesystem, lba, sector);
}

static BOOLEAN delete_directory_slot(LITEOS_FAT32 *filesystem, UINT64 lba,
                                     UINT32 offset) {
    UINT8 sector[4096];
    if (filesystem == 0 || filesystem->BytesPerSector > sizeof(sector) ||
        offset > filesystem->BytesPerSector - FAT32_ENTRY_SIZE ||
        !read_sector(filesystem, lba, sector)) return 0;
    sector[offset] = 0xE5U;
    return write_sector(filesystem, lba, sector);
}

static BOOLEAN read_directory_slot(LITEOS_FAT32 *filesystem, UINT64 lba,
                                   UINT32 offset, UINT8 entry[FAT32_ENTRY_SIZE]) {
    UINT8 sector[4096];
    if (filesystem == 0 || entry == 0 || filesystem->BytesPerSector > sizeof(sector) ||
        offset > filesystem->BytesPerSector - FAT32_ENTRY_SIZE ||
        !read_sector(filesystem, lba, sector)) return 0;
    for (UINT32 i = 0U; i < FAT32_ENTRY_SIZE; ++i) entry[i] = sector[offset + i];
    return 1;
}

static void make_lfn_entry(const CHAR8 *name, UINT32 length, UINT32 count,
                           UINT32 ordinal, UINT8 checksum,
                           UINT8 entry[FAT32_ENTRY_SIZE]) {
    static const UINT8 offsets[13] = {
        1U, 3U, 5U, 7U, 9U, 14U, 16U, 18U, 20U, 22U, 24U, 28U, 30U
    };
    UINT32 base = (ordinal - 1U) * 13U;
    for (UINT32 i = 0U; i < FAT32_ENTRY_SIZE; ++i) entry[i] = 0xFFU;
    entry[0] = (UINT8)ordinal | (ordinal == count ? 0x40U : 0U);
    entry[11] = FAT32_ATTRIBUTE_LFN;
    entry[12] = 0U;
    entry[13] = checksum;
    entry[26] = 0U;
    entry[27] = 0U;
    for (UINT32 i = 0U; i < 13U; ++i) {
        UINT32 index = base + i;
        UINT16 value = index < length ? (UINT16)(UINT8)name[index] :
                       (index == length ? 0U : 0xFFFFU);
        write_u16(entry + offsets[i], value);
    }
}

static BOOLEAN allocate_zero_cluster(LITEOS_FAT32 *filesystem, UINT32 *cluster) {
    UINT8 sector[4096];
    UINT32 value;
    if (filesystem == 0 || cluster == 0 || filesystem->BytesPerSector > sizeof(sector) ||
        !find_free_cluster(filesystem, &value) ||
        !write_file_fat_value(filesystem, value, FAT32_EOC_MIN, 1U)) return 0;
    for (UINT32 byte = 0U; byte < filesystem->BytesPerSector; ++byte) sector[byte] = 0U;
    for (UINT32 sector_index = 0U; sector_index < filesystem->SectorsPerCluster;
         ++sector_index) {
        if (!write_sector(filesystem, cluster_lba(filesystem, value) + sector_index,
                          sector)) {
            (void)free_cluster_chain(filesystem, value);
            return 0;
        }
    }
    *cluster = value;
    return 1;
}

static BOOLEAN initialize_directory_cluster(LITEOS_FAT32 *filesystem, UINT32 cluster,
                                             UINT32 parent) {
    UINT8 sector[4096];
    if (filesystem == 0 || !cluster_valid(filesystem, cluster) ||
        (!root_directory_cluster(filesystem, parent) &&
         !cluster_valid(filesystem, parent)) ||
        filesystem->BytesPerSector > sizeof(sector) ||
        !read_sector(filesystem, cluster_lba(filesystem, cluster), sector)) return 0;
    for (UINT32 i = 0U; i < FAT32_ENTRY_SIZE * 2U; ++i) sector[i] = 0U;
    for (UINT32 i = 0U; i < 11U; ++i) {
        sector[i] = ' ';
        sector[FAT32_ENTRY_SIZE + i] = ' ';
    }
    sector[0] = '.';
    sector[11] = FAT32_DIRECTORY;
    write_u16(sector + 20U, (UINT16)(cluster >> 16));
    write_u16(sector + 26U, (UINT16)cluster);
    sector[FAT32_ENTRY_SIZE] = '.';
    sector[FAT32_ENTRY_SIZE + 1U] = '.';
    sector[FAT32_ENTRY_SIZE + 11U] = FAT32_DIRECTORY;
    write_u16(sector + FAT32_ENTRY_SIZE + 20U, (UINT16)(parent >> 16));
    write_u16(sector + FAT32_ENTRY_SIZE + 26U, (UINT16)parent);
    return write_sector(filesystem, cluster_lba(filesystem, cluster), sector);
}

static BOOLEAN directory_is_empty(LITEOS_FAT32 *filesystem, UINT32 directory) {
    UINT8 sector[4096];
    UINT32 cluster = directory;
    if (filesystem == 0 || !cluster_valid(filesystem, directory) ||
        filesystem->BytesPerSector > sizeof(sector)) return 0;
    for (UINT32 hops = 0U; hops < filesystem->ClusterCount; ++hops) {
        UINT64 first_sector = cluster_lba(filesystem, cluster);
        for (UINT32 sector_index = 0U; sector_index < filesystem->SectorsPerCluster;
             ++sector_index) {
            if (!read_sector(filesystem, first_sector + sector_index, sector)) return 0;
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
        if (!read_next_cluster(filesystem, cluster, &next) || next == 0U) return 1;
        cluster = next;
    }
    return 0;
}

static BOOLEAN fat32_file_open_at(const LITEOS_FAT32 *filesystem, UINT64 lba,
                                  UINT32 offset) {
    if (filesystem == 0) return 0;
    for (UINT32 i = 0U; i < LITEOS_FAT32_MAX_OPEN_FILES; ++i) {
        const LITEOS_FAT32_FILE *file = &filesystem->OpenFiles[i];
        if (file->Used && file->DirectoryLba == lba && file->DirectoryOffset == offset) return 1;
    }
    return 0;
}

static BOOLEAN update_file_directory_entry(LITEOS_FAT32_FILE *file,
                                            UINT32 first_cluster, UINT64 size) {
    LITEOS_FAT32 *filesystem = file == 0 ? 0 : file->FileSystem;
    UINT8 sector[4096];
    UINT8 *entry;
    if (filesystem == 0 || filesystem->BytesPerSector > sizeof(sector) ||
        file->DirectoryOffset > filesystem->BytesPerSector - FAT32_ENTRY_SIZE ||
        size > UINT32_MAX || !read_sector(filesystem, file->DirectoryLba, sector)) {
        return 0;
    }
    entry = sector + file->DirectoryOffset;
    write_u16(entry + 20U, (UINT16)(first_cluster >> 16));
    write_u16(entry + 26U, (UINT16)first_cluster);
    write_u32(entry + 28U, (UINT32)size);
    return write_sector(filesystem, file->DirectoryLba, sector);
}

static BOOLEAN extend_file_to(LITEOS_FAT32_FILE *file, UINT64 required_size) {
    LITEOS_FAT32 *filesystem = file == 0 ? 0 : file->FileSystem;
    UINT64 cluster_bytes;
    UINT64 required_clusters;
    UINT64 existing_clusters = 0U;
    UINT32 old_last = 0U;
    UINT32 new_first = 0U;
    UINT32 new_last = 0U;
    UINT32 orphan = 0U;
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
        if (!find_free_cluster(filesystem, &cluster)) goto fail;
        orphan = cluster;
        if (!write_file_fat_value(filesystem, cluster, FAT32_EOC_MIN, 1U)) goto fail;
        orphan = 0U;
        if (new_first == 0U) {
            new_first = cluster;
        } else if (!write_file_fat_value(filesystem, new_last, cluster, 1U)) {
            orphan = cluster;
            goto fail;
        }
        new_last = cluster;
    }
    if (old_last != 0U) {
        if (!write_file_fat_value(filesystem, old_last, new_first, 1U)) goto fail;
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
    if (attached) (void)write_file_fat_value(filesystem, old_last,
                                               FAT32_EOC_MIN, 1U);
    if (new_first != 0U) (void)free_cluster_chain(filesystem, new_first);
    if (orphan != 0U) (void)write_file_fat_value(filesystem, orphan, 0U, 0U);
    return 0;
}

static BOOLEAN truncate_file_to(LITEOS_FAT32_FILE *file, UINT64 required_size) {
    LITEOS_FAT32 *filesystem = file == 0 ? 0 : file->FileSystem;
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
            !free_cluster_chain(filesystem, file->FirstCluster)) return 0;
        file->FirstCluster = 0U;
    } else {
        if (!locate_file_cluster(file, (UINT32)(required_clusters - 1U), &new_last) ||
            !read_next_cluster(filesystem, new_last, &tail) ||
            !write_file_fat_value(filesystem, new_last, FAT32_EOC_MIN, 1U) ||
            (tail != 0U && !free_cluster_chain(filesystem, tail)) ||
            !update_file_directory_entry(file, file->FirstCluster, required_size)) {
            return 0;
        }
    }
    file->Size = required_size;
    file->CursorValid = 0;
    return 1;
}

static BOOLEAN fat32_read(LITEOS_VFS_NODE *node, UINT64 offset, VOID *buffer,
                          UINT32 capacity, UINT32 *read_size) {
    LITEOS_FAT32_FILE *file = node == 0 ? 0 : (LITEOS_FAT32_FILE *)node->FileContext;
    LITEOS_FAT32 *filesystem = file == 0 ? 0 : file->FileSystem;
    UINT8 sector[4096];
    UINT64 remaining;
    UINT8 *destination = (UINT8 *)buffer;
    if (file == 0 || filesystem == 0 || !file->Used || buffer == 0 || read_size == 0 ||
        capacity == 0U || offset > file->Size || filesystem->BytesPerSector > sizeof(sector)) return 0;
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
        if (!locate_file_cluster(file, logical_cluster, &physical_cluster) ||
            !read_sector(filesystem, cluster_lba(filesystem, physical_cluster) +
                         in_cluster / filesystem->BytesPerSector, sector)) return 0;
        copy_size = filesystem->BytesPerSector - sector_offset;
        if (copy_size > remaining - *read_size) copy_size = (UINT32)(remaining - *read_size);
        for (UINT32 i = 0; i < copy_size; ++i) destination[*read_size + i] = sector[sector_offset + i];
        *read_size += copy_size;
    }
    return 1;
}

static BOOLEAN fat32_write(LITEOS_VFS_NODE *node, UINT64 offset, const VOID *buffer,
                           UINT32 size, UINT32 *written_size) {
    LITEOS_FAT32_FILE *file = node == 0 ? 0 : (LITEOS_FAT32_FILE *)node->FileContext;
    LITEOS_FAT32 *filesystem = file == 0 ? 0 : file->FileSystem;
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
        lba = cluster_lba(filesystem, physical_cluster) +
              in_cluster / filesystem->BytesPerSector;
        /* A complete sector overwrite does not need a read-modify-write cycle. */
        if ((sector_offset != 0U || copy_size != filesystem->BytesPerSector) &&
            !read_sector(filesystem, lba, sector)) return 0;
        for (UINT32 i = 0; i < copy_size; ++i) sector[sector_offset + i] = source[*written_size + i];
        if (!liteos_block_cache_write(&filesystem->Cache, lba, sector)) return 0;
        *written_size += copy_size;
    }
    return 1;
}

static BOOLEAN fat32_close(LITEOS_VFS_NODE *node) {
    LITEOS_FAT32_FILE *file = node == 0 ? 0 : (LITEOS_FAT32_FILE *)node->FileContext;
    if (file == 0 || !file->Used) return 0;
    file->Used = 0;
    return 1;
}

static const LITEOS_VFS_FILE_OPERATIONS g_fat32_operations = {
    fat32_read,
    fat32_write,
    fat32_close,
};

BOOLEAN liteos_fat32_read_path(LITEOS_FAT32 *filesystem, const CHAR8 *path,
                                UINT64 offset, VOID *buffer, UINT32 capacity,
                                UINT32 *read_size) {
    LITEOS_VFS_NODE node = {0};
    BOOLEAN success;
    if (filesystem == 0 || path == 0 || read_size == 0) return 0;
    if (capacity == 0U) {
        *read_size = 0;
        return 1;
    }
    if (!liteos_fat32_lookup(filesystem, path, &node) ||
        node.Operations == 0 || node.Operations->Read == 0) return 0;
    success = node.Operations->Read(&node, offset, buffer, capacity, read_size);
    if (node.Operations->Close != 0 && !node.Operations->Close(&node)) success = 0;
    return success;
}

BOOLEAN liteos_fat32_write_path(LITEOS_FAT32 *filesystem, const CHAR8 *path,
                                 UINT64 offset, const VOID *buffer, UINT32 size,
                                 UINT32 *written_size) {
    LITEOS_VFS_NODE node = {0};
    BOOLEAN success;
    if (filesystem == 0 || path == 0 || written_size == 0) return 0;
    if (size == 0U) {
        *written_size = 0;
        return 1;
    }
    if (!liteos_fat32_lookup(filesystem, path, &node) ||
        node.Operations == 0 || node.Operations->Write == 0) return 0;
    success = node.Operations->Write(&node, offset, buffer, size, written_size);
    if (node.Operations->Close != 0 && !node.Operations->Close(&node)) success = 0;
    return success;
}

BOOLEAN liteos_fat32_truncate_path(LITEOS_FAT32 *filesystem, const CHAR8 *path,
                                    UINT64 size) {
    LITEOS_VFS_NODE node = {0};
    LITEOS_FAT32_FILE *file;
    BOOLEAN success;
    if (filesystem == 0 || path == 0 || size > UINT32_MAX ||
        !liteos_fat32_lookup(filesystem, path, &node) ||
        node.FileContext == 0) return 0;
    file = (LITEOS_FAT32_FILE *)node.FileContext;
    success = truncate_file_to(file, size);
    if (node.Operations != 0 && node.Operations->Close != 0 &&
        !node.Operations->Close(&node)) success = 0;
    return success;
}

BOOLEAN liteos_fat32_stat_path(LITEOS_FAT32 *filesystem, const CHAR8 *path,
                                os_file_info_t *info) {
    UINT8 entry[32];
    UINT32 path_length = 0U;
    UINT32 position = 0U;
    if (filesystem == 0 || !filesystem->Mounted || path == 0 || info == 0) return 0;
    while (path_length < LITEOS_VFS_PATH_LENGTH && path[path_length] != 0) ++path_length;
    if (path_length >= LITEOS_VFS_PATH_LENGTH) return 0;
    while (position < path_length && path[position] == '/') ++position;
    if (position == path_length) {
        *info = (os_file_info_t){0};
        info->type = OS_FILE_TYPE_DIRECTORY;
        info->mode = 0040755U;
        info->name[0] = '/';
        return 1;
    }
    if (!resolve_path(filesystem, path + position, entry, 0, 0)) return 0;
    *info = (os_file_info_t){0};
    info->type = (entry[11] & FAT32_DIRECTORY) != 0U ?
        OS_FILE_TYPE_DIRECTORY : OS_FILE_TYPE_REGULAR;
    info->mode = info->type == OS_FILE_TYPE_DIRECTORY ? 0040755U : 0100666U;
    info->size = info->type == OS_FILE_TYPE_DIRECTORY ? 0U : read_u32(entry + 28U);
    UINT32 start = path_length;
    while (start > 0U && path[start - 1U] != '/') --start;
    UINT32 name_length = path_length - start;
    if (name_length >= OS_FILE_NAME_MAX) name_length = OS_FILE_NAME_MAX - 1U;
    for (UINT32 index = 0U; index < name_length; ++index) {
        info->name[index] = path[start + index];
    }
    info->name[name_length] = 0;
    return 1;
}

BOOLEAN liteos_fat32_enumerate_path(LITEOS_FAT32 *filesystem, const CHAR8 *path,
                                     UINT32 index, os_file_info_t *info) {
    UINT32 directory;
    UINT32 found = 0U;
    if (filesystem == 0 || info == 0 || !resolve_directory_cluster(filesystem, path,
                                                                       &directory)) {
        return 0;
    }
    UINT8 sector[4096];
    UINT16 lfn_characters[260];
    BOOLEAN lfn_valid = 0;
    UINT32 lfn_expected = 0U;
    UINT8 lfn_checksum = 0U;
    UINT32 found_lfn_count = 0U;
    lfn_reset(lfn_characters, &lfn_valid, &lfn_expected, &lfn_checksum);
    for (UINT32 chain = 0U; chain < filesystem->ClusterCount; ++chain) {
        BOOLEAN fixed_root = root_directory_cluster(filesystem, directory);
        if ((!fixed_root && !cluster_valid(filesystem, directory)) ||
            filesystem->BytesPerSector > sizeof(sector)) return 0;
        UINT64 first_sector = fixed_root ? filesystem->RootDirectoryLba :
                           cluster_lba(filesystem, directory);
        UINT32 sector_count = fixed_root ? filesystem->RootDirectorySectors :
                            filesystem->SectorsPerCluster;
        for (UINT32 sector_index = 0U; sector_index < sector_count;
             ++sector_index) {
            if (!read_sector(filesystem, first_sector + sector_index, sector)) return 0;
            for (UINT32 offset = 0U; offset < filesystem->BytesPerSector;
                 offset += FAT32_ENTRY_SIZE) {
                const UINT8 *entry = sector + offset;
                if (entry[0] == 0U) return 0;
                if (entry[0] == 0xE5U) {
                    lfn_reset(lfn_characters, &lfn_valid, &lfn_expected, &lfn_checksum);
                    found_lfn_count = 0U;
                    continue;
                }
                if (entry[11] == FAT32_ATTRIBUTE_LFN) {
                    if ((entry[0] & 0x40U) != 0U) found_lfn_count = 0U;
                    if (found_lfn_count < FAT32_MAX_LFN_ENTRIES) ++found_lfn_count;
                    lfn_store_entry(entry, lfn_characters, &lfn_valid,
                                    &lfn_expected, &lfn_checksum);
                    continue;
                }
                if ((entry[11] & 0x08U) != 0U || fat32_dot_entry(entry)) {
                    lfn_reset(lfn_characters, &lfn_valid, &lfn_expected, &lfn_checksum);
                    found_lfn_count = 0U;
                    continue;
                }
                if (found == index) {
                    *info = (os_file_info_t){0};
                    info->type = (entry[11] & FAT32_DIRECTORY) != 0U ?
                        OS_FILE_TYPE_DIRECTORY : OS_FILE_TYPE_REGULAR;
                    info->mode = info->type == OS_FILE_TYPE_DIRECTORY ?
                        0040755U : 0100666U;
                    info->size = info->type == OS_FILE_TYPE_DIRECTORY ?
                        0U : read_u32(entry + 28U);
                    if (lfn_valid && lfn_expected == 0U &&
                        short_name_checksum(entry) == lfn_checksum) {
                        fat32_lfn_name_text(lfn_characters, info->name);
                    } else {
                        fat32_short_name_text(entry, info->name);
                    }
                    return 1;
                }
                ++found;
                lfn_reset(lfn_characters, &lfn_valid, &lfn_expected, &lfn_checksum);
                found_lfn_count = 0U;
            }
        }
        if (fixed_root) return 0;
        UINT32 next;
        if (!read_next_cluster(filesystem, directory, &next) || next == 0U) return 0;
        directory = next;
    }
    return 0;
}

static BOOLEAN make_creation_name(LITEOS_FAT32 *filesystem, UINT32 parent,
                                  const CHAR8 *leaf, UINT32 length,
                                  UINT8 short_name[11], UINT32 *lfn_count) {
    if (filesystem == 0 || leaf == 0 || short_name == 0 || lfn_count == 0 ||
        length == 0U || length > 255U) return 0;
    if (make_short_name(leaf, length, short_name)) {
        *lfn_count = 0U;
        return 1;
    }
    *lfn_count = (length + 12U) / 13U;
    if (*lfn_count == 0U || *lfn_count > FAT32_MAX_LFN_ENTRIES) return 0;
    for (UINT32 suffix = 1U; suffix < 1000U; ++suffix) {
        if (!make_long_name_alias(leaf, length, suffix, short_name)) return 0;
        if (!directory_short_name_exists(filesystem, parent, short_name)) return 1;
    }
    return 0;
}

BOOLEAN liteos_fat32_create_path(LITEOS_FAT32 *filesystem, const CHAR8 *path,
                                  BOOLEAN directory) {
    CHAR8 leaf[LITEOS_VFS_PATH_LENGTH];
    UINT8 entry[FAT32_ENTRY_SIZE];
    UINT8 short_name[11];
    UINT8 lfn_entry[FAT32_ENTRY_SIZE];
    UINT64 slot_lba[FAT32_MAX_CREATE_SLOTS];
    UINT32 slot_offset[FAT32_MAX_CREATE_SLOTS];
    UINT32 new_clusters[FAT32_MAX_CREATE_SLOTS];
    UINT32 parent;
    UINT32 leaf_length = 0U;
    UINT32 lfn_count;
    UINT32 slot_count;
    UINT32 new_cluster_count = 0U;
    UINT32 extension_anchor = 0U;
    UINT32 written_slots = 0U;
    UINT32 first_cluster = 0U;
    if (!resolve_parent_directory(filesystem, path, &parent, leaf) ||
        (!cluster_valid(filesystem, parent) &&
         !root_directory_cluster(filesystem, parent))) return 0;
    while (leaf_length < LITEOS_VFS_PATH_LENGTH && leaf[leaf_length] != 0) ++leaf_length;
    if (leaf_length == 0U || leaf_length > 255U ||
        find_directory_entry(filesystem, parent, leaf, leaf_length, entry, 0, 0) ||
        !make_creation_name(filesystem, parent, leaf, leaf_length, short_name,
                            &lfn_count)) return 0;
    slot_count = lfn_count + 1U;
    if (!find_directory_slots(filesystem, parent, slot_count, slot_lba, slot_offset,
                              new_clusters, &new_cluster_count, &extension_anchor)) return 0;

    for (UINT32 i = 0U; i < FAT32_ENTRY_SIZE; ++i) entry[i] = 0U;
    for (UINT32 i = 0U; i < 11U; ++i) entry[i] = short_name[i];
    entry[11] = directory ? FAT32_DIRECTORY : 0x20U;
    if (directory) {
        if (!allocate_zero_cluster(filesystem, &first_cluster) ||
            !initialize_directory_cluster(filesystem, first_cluster, parent)) {
            if (first_cluster != 0U) (void)free_cluster_chain(filesystem, first_cluster);
            rollback_directory_extensions(filesystem, extension_anchor, new_clusters,
                                          new_cluster_count);
            return 0;
        }
    }
    write_u16(entry + 20U, (UINT16)(first_cluster >> 16));
    write_u16(entry + 26U, (UINT16)first_cluster);
    write_u32(entry + 28U, 0U);
    for (UINT32 ordinal = lfn_count; ordinal != 0U; --ordinal) {
        make_lfn_entry(leaf, leaf_length, lfn_count, ordinal,
                       short_name_checksum(short_name), lfn_entry);
        UINT32 slot = lfn_count - ordinal;
        if (!write_directory_slot(filesystem, slot_lba[slot], slot_offset[slot],
                                  lfn_entry)) break;
        ++written_slots;
    }
    if (written_slots == lfn_count &&
        write_directory_slot(filesystem, slot_lba[lfn_count], slot_offset[lfn_count],
                             entry)) {
        return 1;
    }
    for (UINT32 i = 0U; i < written_slots; ++i) {
        (void)delete_directory_slot(filesystem, slot_lba[i], slot_offset[i]);
    }
    if (written_slots == lfn_count) {
        (void)delete_directory_slot(filesystem, slot_lba[lfn_count],
                                     slot_offset[lfn_count]);
    }
    if (first_cluster != 0U) (void)free_cluster_chain(filesystem, first_cluster);
    rollback_directory_extensions(filesystem, extension_anchor, new_clusters,
                                  new_cluster_count);
    return 0;
}

BOOLEAN liteos_fat32_remove_path(LITEOS_FAT32 *filesystem, const CHAR8 *path) {
    CHAR8 leaf[LITEOS_VFS_PATH_LENGTH];
    UINT8 entry[FAT32_ENTRY_SIZE];
    UINT64 lfn_lba[FAT32_MAX_LFN_ENTRIES];
    UINT32 lfn_offset[FAT32_MAX_LFN_ENTRIES];
    UINT32 parent;
    UINT32 leaf_length = 0U;
    UINT64 entry_lba = 0U;
    UINT32 entry_offset = 0U;
    UINT32 lfn_count = 0U;
    UINT32 first_cluster;
    UINT8 lfn_entries[FAT32_MAX_LFN_ENTRIES][FAT32_ENTRY_SIZE];
    UINT8 short_entry[FAT32_ENTRY_SIZE];
    UINT32 deleted_lfn = 0U;
    if (!resolve_parent_directory(filesystem, path, &parent, leaf) ||
        filesystem->BytesPerSector > LITEOS_BLOCK_CACHE_MAX_SIZE) return 0;
    while (leaf_length < LITEOS_VFS_PATH_LENGTH && leaf[leaf_length] != 0) ++leaf_length;
    if (leaf_length == 0U || (leaf[0] == '.' && (leaf_length == 1U || leaf[1] == '.'))) {
        return 0;
    }
    if (!find_directory_entry_ex(filesystem, parent, leaf, leaf_length, entry,
                                 &entry_lba, &entry_offset, lfn_lba, lfn_offset,
                                 &lfn_count) ||
        fat32_file_open_at(filesystem, entry_lba, entry_offset)) return 0;
    first_cluster = entry_cluster(entry);
    if ((entry[11] & FAT32_DIRECTORY) != 0U) {
        if (first_cluster == 0U || !directory_is_empty(filesystem, first_cluster)) return 0;
    }
    if (!read_directory_slot(filesystem, entry_lba, entry_offset, short_entry)) return 0;
    for (UINT32 i = 0U; i < lfn_count; ++i) {
        if (!read_directory_slot(filesystem, lfn_lba[i], lfn_offset[i], lfn_entries[i])) {
            return 0;
        }
    }
    /* 先隐藏短名，避免中途失败时查找器看到没有对应短名的 LFN。 */
    if (!delete_directory_slot(filesystem, entry_lba, entry_offset)) return 0;
    for (UINT32 i = 0U; i < lfn_count; ++i) {
        if (!delete_directory_slot(filesystem, lfn_lba[i], lfn_offset[i])) {
            for (UINT32 restore = 0U; restore < deleted_lfn; ++restore) {
                (void)write_directory_slot(filesystem, lfn_lba[restore],
                                            lfn_offset[restore], lfn_entries[restore]);
            }
            (void)write_directory_slot(filesystem, entry_lba, entry_offset, short_entry);
            return 0;
        }
        ++deleted_lfn;
    }
    if (first_cluster == 0U || free_cluster_chain(filesystem, first_cluster)) return 1;

    /* FAT 释放失败时，目录项也必须恢复，否则文件会变成不可达对象。 */
    for (UINT32 restore = 0U; restore < lfn_count; ++restore) {
        (void)write_directory_slot(filesystem, lfn_lba[restore], lfn_offset[restore],
                                    lfn_entries[restore]);
    }
    (void)write_directory_slot(filesystem, entry_lba, entry_offset, short_entry);
    return 0;
}

BOOLEAN liteos_fat32_sync(LITEOS_FAT32 *filesystem) {
    return filesystem != 0 && filesystem->Mounted &&
           liteos_block_cache_flush(&filesystem->Cache);
}

BOOLEAN liteos_fat32_init(LITEOS_FAT32 *filesystem,
                          LITEOS_BLOCK_DEVICE *device) {
    UINT8 boot_sector[4096];
    LITEOS_FAT32 boot_filesystem;
    UINT32 total_sectors16;
    UINT32 total_sectors32;
    UINT64 total_sectors;
    UINT32 root_entries;
    UINT32 fat_sectors16;
    UINT32 root_directory_sectors;
    UINT32 fat_type;
    UINT64 data_sectors;
    UINT32 fat1_zero;
    UINT32 fat2_zero;
    UINT32 fat1_one;
    UINT32 fat2_one;
    if (filesystem == 0 || device == 0 || filesystem->Mounted ||
        device->BlockSize < 512U || device->BlockSize > sizeof(boot_sector) ||
        !is_power_of_two(device->BlockSize)) return 0;
    boot_filesystem.Device = device;
    boot_filesystem.BytesPerSector = device->BlockSize;
    boot_filesystem.Cache.Initialized = 0;
    if (!read_sector(&boot_filesystem, 0, boot_sector)) return 0;
    if (boot_sector[510] != 0x55U || boot_sector[511] != 0xAAU) return 0;
    UINT32 bytes_per_sector = read_u16(boot_sector + 11U);
    UINT32 sectors_per_cluster = boot_sector[13];
    UINT32 reserved = read_u16(boot_sector + 14U);
    UINT32 fat_count = boot_sector[16];
    root_entries = read_u16(boot_sector + 17U);
    fat_sectors16 = read_u16(boot_sector + 22U);
    UINT32 fat_sectors = read_u32(boot_sector + 36U);
    UINT32 root_cluster = read_u32(boot_sector + 44U);
    total_sectors16 = read_u16(boot_sector + 19U);
    total_sectors32 = read_u32(boot_sector + 32U);
    total_sectors = total_sectors16 != 0U ? total_sectors16 : total_sectors32;
    if (bytes_per_sector == 0U) return 0;
    fat_type = fat_sectors16 != 0U && root_entries != 0U ? FAT_TYPE_16 : FAT_TYPE_32;
    if (fat_type == FAT_TYPE_16) {
        fat_sectors = fat_sectors16;
        root_cluster = 0U;
        root_directory_sectors =
            ((root_entries * FAT32_ENTRY_SIZE) + bytes_per_sector - 1U) /
            bytes_per_sector;
    } else {
        root_directory_sectors = 0U;
    }
    if (bytes_per_sector != device->BlockSize || !is_power_of_two(bytes_per_sector) ||
        sectors_per_cluster == 0U || !is_power_of_two(sectors_per_cluster) || reserved == 0U ||
        fat_count < 2U || fat_sectors == 0U || total_sectors == 0 ||
        ((fat_type == FAT_TYPE_32 && root_cluster < 2U) ||
         (fat_type == FAT_TYPE_16 && root_entries == 0U)) ||
        (UINT64)reserved + (UINT64)fat_count * fat_sectors + root_directory_sectors >=
            total_sectors) return 0;
    data_sectors = total_sectors - reserved - (UINT64)fat_count * fat_sectors -
                   root_directory_sectors;
    if (data_sectors / sectors_per_cluster == 0 || data_sectors / sectors_per_cluster > 0x0FFFFFF0ULL ||
        total_sectors > device->BlockCount) return 0;
    filesystem->Device = device;
    filesystem->BytesPerSector = bytes_per_sector;
    filesystem->SectorsPerCluster = sectors_per_cluster;
    filesystem->ReservedSectorCount = reserved;
    filesystem->FatCount = fat_count;
    filesystem->FatSectors = fat_sectors;
    filesystem->RootCluster = root_cluster;
    filesystem->RootDirectorySectors = root_directory_sectors;
    filesystem->ClusterCount = (UINT32)(data_sectors / sectors_per_cluster);
    filesystem->FatStartLba = reserved;
    filesystem->RootDirectoryLba = reserved + (UINT64)fat_count * fat_sectors;
    filesystem->DataStartLba = filesystem->RootDirectoryLba + root_directory_sectors;
    filesystem->Fat3StartLba = reserved + (UINT64)2U * fat_sectors;
    filesystem->Fat3Available = fat_type == FAT_TYPE_32 && fat_count >= 3U;
    filesystem->FatType = fat_type;
    if (!liteos_block_cache_init(&filesystem->Cache, device)) return 0;
    if ((!root_directory_cluster(filesystem, root_cluster) &&
         !cluster_valid(filesystem, root_cluster)) ||
        !read_fat_entry(filesystem, 1U, 0U + 2U, &fat1_zero) ||
        !read_fat_entry(filesystem, 2U, 0U + 2U, &fat2_zero) ||
        !read_fat_entry(filesystem, 1U, 3U, &fat1_one) ||
        !read_fat_entry(filesystem, 2U, 3U, &fat2_one) ||
        fat1_zero != fat2_zero || fat1_one != fat2_one) {
        liteos_block_cache_destroy(&filesystem->Cache);
        return 0;
    }
    for (UINT32 i = 0; i < LITEOS_FAT32_MAX_OPEN_FILES; ++i) filesystem->OpenFiles[i].Used = 0;
    filesystem->Mounted = 1;
    return 1;
}

BOOLEAN liteos_fat32_destroy(LITEOS_FAT32 *filesystem) {
    if (filesystem == 0 || !filesystem->Mounted) return 0;
    for (UINT32 i = 0; i < LITEOS_FAT32_MAX_OPEN_FILES; ++i) {
        if (filesystem->OpenFiles[i].Used) return 0;
    }
    if (!liteos_block_cache_destroy(&filesystem->Cache)) return 0;
    filesystem->Mounted = 0;
    filesystem->Device = 0;
    return 1;
}

BOOLEAN liteos_fat32_lookup(VOID *filesystem_context, const CHAR8 *path,
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
