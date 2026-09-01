#include "internal.h"

/* REFACTOR_FS_FAT32_FAT_TABLE_OWNER: media I/O, FAT entries and cluster chains. */

#define FAT_TABLE_EOC_MIN       0x0FFFFFF8U
#define FAT_TABLE_BAD_CLUSTER   0x0FFFFFF7U
#define FAT_TABLE16_EOC_MIN     0x0000FFF8U
#define FAT_TABLE16_BAD_CLUSTER 0x0000FFF7U
#define FAT_TABLE_TYPE_16       16U

BOOLEAN fat32_cluster_valid(const LITEOS_FAT32 *filesystem, UINT32 cluster) {
    return filesystem != 0 && cluster >= 2U &&
           cluster <= filesystem->ClusterCount + 1U;
}

BOOLEAN fat32_root_directory_cluster(const LITEOS_FAT32 *filesystem,
                                      UINT32 cluster) {
    return filesystem != 0 && filesystem->FatType == FAT_TABLE_TYPE_16 &&
           cluster == 0U;
}

BOOLEAN fat32_read_sector(const LITEOS_FAT32 *filesystem, UINT64 lba,
                          UINT8 *buffer) {
    if (filesystem == 0 || buffer == 0 || filesystem->Device == 0 ||
        lba >= filesystem->Device->BlockCount) return 0;
    /*
    * The NVMe backend is asynchronous even for this synchronous FAT API.
     * A completion can race the deferred queue consumer and transiently
     * return an I/O error. Retry the same sector while the request is local.
    */
    for (UINT32 attempt = 0U; attempt < 3U; ++attempt) {
        BOOLEAN success = filesystem->Cache.Initialized ?
            liteos_block_cache_read((LITEOS_BLOCK_CACHE *)&filesystem->Cache,
                                    lba, buffer) :
            liteos_block_read(filesystem->Device, lba, 1U, buffer);
        if (success) return 1;
        __asm__ volatile ("pause");
    }
    return 0;
}

BOOLEAN fat32_write_sector(LITEOS_FAT32 *filesystem, UINT64 lba,
                           const UINT8 *buffer) {
    if (filesystem == 0 || buffer == 0 || filesystem->Device == 0 ||
        lba >= filesystem->Device->BlockCount) return 0;
    if (filesystem->Cache.Initialized) {
        return liteos_block_cache_write(&filesystem->Cache, lba, buffer);
    }
    return liteos_block_write(filesystem->Device, lba, 1U, buffer);
}

UINT64 fat32_cluster_lba(const LITEOS_FAT32 *filesystem, UINT32 cluster) {
    return filesystem->DataStartLba +
           (UINT64)(cluster - 2U) * filesystem->SectorsPerCluster;
}

BOOLEAN fat32_read_fat_entry(const LITEOS_FAT32 *filesystem, UINT32 fat_number,
                             UINT32 cluster, UINT32 *value) {
    UINT8 sector[4096];
    UINT64 byte_offset;
    UINT64 lba;
    UINT32 offset;
    UINT32 entry_size;
    UINT64 fat_start;
    if (filesystem == 0 || value == 0 ||
        !fat32_cluster_valid(filesystem, cluster) || fat_number == 0U ||
        fat_number > filesystem->FatCount) return 0;
    entry_size = filesystem->FatType == FAT_TABLE_TYPE_16 ?
                 sizeof(UINT16) : sizeof(UINT32);
    byte_offset = (UINT64)cluster * entry_size;
    lba = byte_offset / filesystem->BytesPerSector;
    offset = (UINT32)(byte_offset % filesystem->BytesPerSector);
    if (filesystem->BytesPerSector > sizeof(sector) ||
        offset + entry_size > filesystem->BytesPerSector) return 0;
    fat_start = filesystem->FatStartLba +
                (UINT64)(fat_number - 1U) * filesystem->FatSectors;
    if (!fat32_read_sector(filesystem, fat_start + lba, sector)) return 0;
    *value = filesystem->FatType == FAT_TABLE_TYPE_16 ?
             fat32_read_u16(sector + offset) :
             fat32_read_u32(sector + offset) & 0x0FFFFFFFU;
    return 1;
}

BOOLEAN fat32_write_fat_entry(LITEOS_FAT32 *filesystem, UINT32 fat_number,
                              UINT32 cluster, UINT32 value) {
    UINT8 sector[4096];
    UINT64 byte_offset;
    UINT64 lba;
    UINT32 offset;
    UINT32 entry_size;
    UINT64 fat_start;
    if (filesystem == 0 ||
        !fat32_cluster_valid(filesystem, cluster) || fat_number == 0U ||
        fat_number > filesystem->FatCount ||
        filesystem->BytesPerSector > sizeof(sector)) return 0;
    entry_size = filesystem->FatType == FAT_TABLE_TYPE_16 ?
                 sizeof(UINT16) : sizeof(UINT32);
    byte_offset = (UINT64)cluster * entry_size;
    lba = byte_offset / filesystem->BytesPerSector;
    offset = (UINT32)(byte_offset % filesystem->BytesPerSector);
    if (offset + entry_size > filesystem->BytesPerSector) return 0;
    fat_start = filesystem->FatStartLba +
                (UINT64)(fat_number - 1U) * filesystem->FatSectors;
    if (!fat32_read_sector(filesystem, fat_start + lba, sector)) return 0;
    if (filesystem->FatType == FAT_TABLE_TYPE_16) {
        fat32_write_u16(sector + offset, (UINT16)value);
    } else {
        fat32_write_u32(sector + offset, value & 0x0FFFFFFFU);
    }
    return fat32_write_sector(filesystem, fat_start + lba, sector);
}

BOOLEAN fat32_write_file_fat_value(LITEOS_FAT32 *filesystem, UINT32 cluster,
                                   UINT32 chain_value, UINT32 extent_value) {
    for (UINT32 fat = 1U; fat <= filesystem->FatCount; ++fat) {
        UINT32 value = filesystem->Fat3Available && fat == 3U ?
                       extent_value : chain_value;
        if (!fat32_write_fat_entry(filesystem, fat, cluster, value)) return 0;
    }
    return 1;
}

BOOLEAN fat32_find_free_cluster(LITEOS_FAT32 *filesystem, UINT32 *cluster) {
    UINT32 value;
    if (filesystem == 0 || cluster == 0) return 0;
    for (UINT32 candidate = 2U;
         candidate <= filesystem->ClusterCount + 1U; ++candidate) {
        if (!fat32_read_fat_entry(filesystem, 1U, candidate, &value)) return 0;
        if (value == 0U) {
            *cluster = candidate;
            return 1;
        }
    }
    return 0;
}

BOOLEAN fat32_read_next_cluster(const LITEOS_FAT32 *filesystem, UINT32 cluster,
                                UINT32 *next) {
    UINT32 value;
    UINT32 eoc_min = filesystem != 0 && filesystem->FatType == FAT_TABLE_TYPE_16 ?
                     FAT_TABLE16_EOC_MIN : FAT_TABLE_EOC_MIN;
    UINT32 bad_cluster = filesystem != 0 &&
                         filesystem->FatType == FAT_TABLE_TYPE_16 ?
                         FAT_TABLE16_BAD_CLUSTER : FAT_TABLE_BAD_CLUSTER;
    if (!fat32_read_fat_entry(filesystem, 1U, cluster, &value) ||
        value == bad_cluster) return 0;
    if (value >= eoc_min) {
        *next = 0U;
        return 1;
    }
    if (!fat32_cluster_valid(filesystem, value)) return 0;
    *next = value;
    return 1;
}

UINT32 fat32_extent_length(const LITEOS_FAT32 *filesystem, UINT32 cluster) {
    UINT32 length;
    if (!filesystem->Fat3Available ||
        !fat32_read_fat_entry(filesystem, 3U, cluster, &length) ||
        length == 0U || length > filesystem->ClusterCount + 2U - cluster) {
        return 1U;
    }
    return length;
}
