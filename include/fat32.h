#ifndef LITEOS_FAT32_H
#define LITEOS_FAT32_H

#include "block.h"
#include "cache.h"
#include "vfs.h"
#include <uapi/file.h>

#define LITEOS_FAT32_MAX_OPEN_FILES 16U

typedef struct LITEOS_FAT32 LITEOS_FAT32;

typedef struct {
    BOOLEAN Used;
    LITEOS_FAT32 *FileSystem;
    UINT32 FirstCluster;
    UINT64 Size;
    UINT32 Attributes;
    /* 目录项位置用于扩展文件后更新大小和首簇。 */
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
    LITEOS_FAT32_FILE OpenFiles[LITEOS_FAT32_MAX_OPEN_FILES];
    BOOLEAN Mounted;
};

BOOLEAN liteos_fat32_init(LITEOS_FAT32 *filesystem,
                          LITEOS_BLOCK_DEVICE *device);
BOOLEAN liteos_fat32_destroy(LITEOS_FAT32 *filesystem);
BOOLEAN liteos_fat32_lookup(VOID *filesystem_context, const CHAR8 *path,
                            LITEOS_VFS_NODE *node);
BOOLEAN liteos_fat32_stat_path(LITEOS_FAT32 *filesystem, const CHAR8 *path,
                                os_file_info_t *info);
BOOLEAN liteos_fat32_enumerate_path(LITEOS_FAT32 *filesystem, const CHAR8 *path,
                                     UINT32 index, os_file_info_t *info);
/* 为规范 VFS 提供按路径、按偏移的后端访问。 */
BOOLEAN liteos_fat32_read_path(LITEOS_FAT32 *filesystem, const CHAR8 *path,
                                UINT64 offset, VOID *buffer, UINT32 capacity,
                                UINT32 *read_size);
BOOLEAN liteos_fat32_write_path(LITEOS_FAT32 *filesystem, const CHAR8 *path,
                                 UINT64 offset, const VOID *buffer, UINT32 size,
                                 UINT32 *written_size);
BOOLEAN liteos_fat32_truncate_path(LITEOS_FAT32 *filesystem, const CHAR8 *path,
                                    UINT64 size);
/* 创建或删除短文件名（8.3）对象；目录创建会初始化 . 与 .. 项。 */
BOOLEAN liteos_fat32_create_path(LITEOS_FAT32 *filesystem, const CHAR8 *path,
                                  BOOLEAN directory);
BOOLEAN liteos_fat32_remove_path(LITEOS_FAT32 *filesystem, const CHAR8 *path);
BOOLEAN liteos_fat32_sync(LITEOS_FAT32 *filesystem);

#endif
