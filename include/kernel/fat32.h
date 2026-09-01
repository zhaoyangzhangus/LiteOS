#pragma once

#include "base.h"
#include "block_device.h"
#include "block_cache.h"
#include <uapi/file.h>

/* FAT32 owns its path and open-file boundary; VFS consumes these APIs. */
#define LITEOS_FAT32_PATH_LENGTH 256U
#define LITEOS_FAT32_MAX_OPEN_FILES 64U

typedef struct LITEOS_FAT32 LITEOS_FAT32;

typedef struct {
    BOOLEAN Used;
    LITEOS_FAT32 *FileSystem;
    UINT32 FirstCluster;
    UINT64 Size;
    UINT32 Attributes;
    UINT64 DirectoryLba;
    UINT32 DirectoryOffset;
    BOOLEAN CursorValid;
    UINT32 CursorLogicalStart;
    UINT32 CursorPhysicalStart;
    UINT32 CursorLength;
} LITEOS_FAT32_FILE;

struct LITEOS_FAT32 {
    LITEOS_BLOCK_DEVICE *Device;
    UINT32 BytesPerSector;
    UINT32 SectorsPerCluster;
    UINT32 ReservedSectorCount;
    UINT32 FatCount;
    UINT32 FatSectors;
    UINT32 RootCluster;
    UINT32 RootDirectorySectors;
    UINT32 ClusterCount;
    UINT64 FatStartLba;
    UINT64 RootDirectoryLba;
    UINT64 DataStartLba;
    UINT64 Fat3StartLba;
    UINT32 FatType;
    BOOLEAN Fat3Available;
    LITEOS_BLOCK_CACHE Cache;
    UINT32 OpenFileLock;
    UINT32 MutationLock;
    LITEOS_FAT32_FILE OpenFiles[LITEOS_FAT32_MAX_OPEN_FILES];
    BOOLEAN Mounted;
};

BOOLEAN liteos_fat32_init(LITEOS_FAT32 *filesystem,
                          LITEOS_BLOCK_DEVICE *device);
BOOLEAN liteos_fat32_destroy(LITEOS_FAT32 *filesystem);
BOOLEAN liteos_fat32_open(LITEOS_FAT32 *filesystem, const CHAR8 *path,
                          LITEOS_FAT32_FILE **out);
BOOLEAN liteos_fat32_close(LITEOS_FAT32_FILE *file);
BOOLEAN liteos_fat32_stat_path(LITEOS_FAT32 *filesystem, const CHAR8 *path,
                                os_file_info_t *info);
BOOLEAN liteos_fat32_enumerate_path(LITEOS_FAT32 *filesystem, const CHAR8 *path,
                                     UINT32 index, os_file_info_t *info);
BOOLEAN liteos_fat32_read_path(LITEOS_FAT32 *filesystem, const CHAR8 *path,
                                UINT64 offset, VOID *buffer, UINT32 capacity,
                                UINT32 *read_size);
BOOLEAN liteos_fat32_write_path(LITEOS_FAT32 *filesystem, const CHAR8 *path,
                                 UINT64 offset, const VOID *buffer, UINT32 size,
                                 UINT32 *written_size);
BOOLEAN liteos_fat32_write_file(LITEOS_FAT32_FILE *file, UINT64 offset,
                                const VOID *buffer, UINT32 size,
                                UINT32 *written_size);
BOOLEAN liteos_fat32_truncate_path(LITEOS_FAT32 *filesystem, const CHAR8 *path,
                                    UINT64 size);
BOOLEAN liteos_fat32_create_path(LITEOS_FAT32 *filesystem, const CHAR8 *path,
                                  BOOLEAN directory);
BOOLEAN liteos_fat32_remove_path(LITEOS_FAT32 *filesystem, const CHAR8 *path);
BOOLEAN liteos_fat32_rename_path(LITEOS_FAT32 *filesystem, const CHAR8 *old_path,
                                  const CHAR8 *new_path);
BOOLEAN liteos_fat32_sync(LITEOS_FAT32 *filesystem);
