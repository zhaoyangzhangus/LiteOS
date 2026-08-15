#include <stdio.h>
#include <string.h>
#include "block.h"
#include "fat32.h"

#define TEST_SECTORS 128U
#define HELLO_DATA_LBA 4U
static UINT8 disk[TEST_SECTORS * 512U];
static UINT32 fail_write_number;
static UINT32 write_count;
static UINT32 read_counts[TEST_SECTORS];

static void put16(UINT8 *p, UINT16 value) {
    p[0] = (UINT8)value;
    p[1] = (UINT8)(value >> 8);
}

static void put32(UINT8 *p, UINT32 value) {
    p[0] = (UINT8)value;
    p[1] = (UINT8)(value >> 8);
    p[2] = (UINT8)(value >> 16);
    p[3] = (UINT8)(value >> 24);
}

static UINT8 short_checksum(const UINT8 name[11]) {
    UINT8 checksum = 0U;
    for (UINT32 i = 0; i < 11U; ++i) {
        checksum = (UINT8)(((checksum & 1U) != 0U ? 0x80U : 0U) +
                           (checksum >> 1) + name[i]);
    }
    return checksum;
}

static void put_lfn_entry(UINT8 *entry, const char *name, size_t length,
                          UINT32 start, UINT8 sequence, UINT8 checksum) {
    static const UINT8 offsets[13] = {
        1U, 3U, 5U, 7U, 9U, 14U, 16U, 18U, 20U, 22U, 24U, 28U, 30U
    };
    memset(entry, 0xFF, 32U);
    entry[0] = sequence;
    entry[11] = 0x0FU;
    entry[13] = checksum;
    for (UINT32 i = 0; i < 13U; ++i) {
        UINT32 index = start + i;
        UINT16 value = index < length ? (UINT8)name[index] :
                       (index == length ? 0U : 0xFFFFU);
        put16(entry + offsets[i], value);
    }
}

static void put_lfn(UINT8 *entry, const char *name, const UINT8 short_name[11]) {
    size_t length = strlen(name);
    UINT8 checksum = short_checksum(short_name);
    UINT32 count = (UINT32)((length + 12U) / 13U);
    for (UINT32 ordinal = count; ordinal != 0U; --ordinal) {
        UINT32 start = (ordinal - 1U) * 13U;
        put_lfn_entry(entry, name, length, start,
                      (UINT8)ordinal | (ordinal == count ? 0x40U : 0U), checksum);
        entry += 32U;
    }
}

static BOOLEAN read_disk(VOID *context, UINT64 lba, UINT32 count, VOID *buffer) {
    UINT8 *data = (UINT8 *)context;
    if (data == 0 || buffer == 0 || lba >= TEST_SECTORS || count > TEST_SECTORS - lba) return 0;
    for (UINT32 index = 0U; index < count; ++index) ++read_counts[(UINT32)lba + index];
    memcpy(buffer, data + lba * 512ULL, count * 512U);
    return 1;
}

static BOOLEAN write_disk(VOID *context, UINT64 lba, UINT32 count, const VOID *buffer) {
    UINT8 *data = (UINT8 *)context;
    if (data == 0 || buffer == 0 || lba >= TEST_SECTORS || count > TEST_SECTORS - lba) return 0;
    ++write_count;
    if (fail_write_number != 0U && write_count == fail_write_number) return 0;
    memcpy(data + lba * 512ULL, buffer, count * 512U);
    return 1;
}

static void make_disk(void) {
    UINT8 *fat1;
    memset(disk, 0, sizeof(disk));
    fail_write_number = 0U;
    write_count = 0U;
    memset(read_counts, 0, sizeof(read_counts));
    disk[510] = 0x55U;
    disk[511] = 0xAAU;
    put16(disk + 11U, 512U);
    disk[13] = 1U;
    put16(disk + 14U, 1U);
    disk[16] = 2U;
    put16(disk + 19U, TEST_SECTORS);
    put32(disk + 36U, 1U);
    put32(disk + 44U, 2U);
    fat1 = disk + 512U;
    put32(fat1 + 0U, 0x0FFFFFF8U);
    put32(fat1 + 4U, 0xFFFFFFFFU);
    put32(fat1 + 8U, 0x0FFFFFFFU);
    put32(fat1 + 12U, 0x0FFFFFFFU);
    /* 簇 4 留作扩展 hello.txt 的空闲簇。 */
    put32(fat1 + 16U, 0U);
    put32(fat1 + 20U, 0x0FFFFFFFU);
    /* cluster 6 保存子目录，cluster 7 保存子目录中的文件。 */
    put32(fat1 + 24U, 0x0FFFFFFFU);
    put32(fat1 + 28U, 0x0FFFFFFFU);
    memcpy(fat1 + 512U, fat1, 512U);
    memcpy(disk + 3U * 512U, "HELLO   TXT", 11U);
    disk[3U * 512U + 11U] = 0x20U;
    put16(disk + 3U * 512U + 26U, 3U);
    put32(disk + 3U * 512U + 28U, 5U);
    memcpy(disk + 4U * 512U, "hello", 5U);

    static const UINT8 long_short_name[] = "LONGFI~1TXT";
    put_lfn(disk + 3U * 512U + 32U, "Long File Name.txt", long_short_name);
    memcpy(disk + 3U * 512U + 96U, long_short_name, 11U);
    disk[3U * 512U + 96U + 11U] = 0x20U;
    put16(disk + 3U * 512U + 96U + 26U, 5U);
    put32(disk + 3U * 512U + 96U + 28U, 9U);
    memcpy(disk + 6U * 512U, "long file", 9U);

    UINT8 *root_directory = disk + 3U * 512U + 128U;
    memcpy(root_directory, "DIR        ", 11U);
    root_directory[11] = 0x10U;
    put16(root_directory + 26U, 6U);
    put32(root_directory + 28U, 0U);
    UINT8 *directory = disk + 7U * 512U;
    UINT8 *nested = directory;
    memcpy(nested, "NESTED  TXT", 11U);
    nested[11] = 0x20U;
    put16(nested + 26U, 7U);
    put32(nested + 28U, 6U);
    memcpy(disk + 8U * 512U, "nested", 6U);
}

int main(void) {
    LITEOS_BLOCK_MANAGER blocks = {0};
    LITEOS_BLOCK_DEVICE *device = 0;
    LITEOS_FAT32 fat = {0};
    LITEOS_VFS_MANAGER vfs = {0};
    LITEOS_FILE file = {0};
    LITEOS_VFS_NODE rollback_node = {0};
    CHAR8 buffer[6] = {0};
    CHAR8 nested_buffer[7] = {0};
    UINT8 extension[600];
    UINT8 full_sector[512];
    UINT32 hello_data_reads;
    UINT32 size = 0;
    for (UINT32 i = 0; i < sizeof(extension); ++i) extension[i] = (UINT8)(i ^ 0x5AU);
    make_disk();
    if (!liteos_block_manager_init(&blocks) ||
        !liteos_block_register(&blocks, "mem0", 512U, TEST_SECTORS,
                               read_disk, write_disk, 0, disk, &device)) return 1;
    if (!liteos_fat32_init(&fat, device)) return 2;
    if (!liteos_vfs_init(&vfs) || !liteos_vfs_mount(&vfs, "/", liteos_fat32_lookup, &fat) ||
        !liteos_vfs_open(&vfs, "/hello.txt", &file)) return 3;
    if (!liteos_vfs_read(&file, buffer, 5U, &size) || size != 5U || strcmp(buffer, "hello") != 0) return 4;
    if (!liteos_vfs_close(&file) ||
        !liteos_vfs_open(&vfs, "/Long File Name.txt", &file) ||
        !liteos_vfs_read(&file, buffer, 5U, &size) || size != 5U ||
        memcmp(buffer, "long ", 5U) != 0 || !liteos_vfs_close(&file) ||
        !liteos_vfs_open(&vfs, "/DIR/NESTED.TXT", &file) ||
        !liteos_vfs_read(&file, nested_buffer, 6U, &size) || size != 6U ||
        memcmp(nested_buffer, "nested", 6U) != 0 ||
        !liteos_vfs_close(&file) ||
        !liteos_vfs_open_access(&vfs, "/hello.txt", LITEOS_ACCESS_READ | LITEOS_ACCESS_WRITE, &file) ||
        !liteos_vfs_write(&file, "world", 5U, &size) || size != 5U || !liteos_vfs_close(&file) ||
        !liteos_vfs_open_access(&vfs, "/hello.txt", LITEOS_ACCESS_READ | LITEOS_ACCESS_WRITE, &file)) return 5;
    memset(full_sector, 0xA5, sizeof(full_sector));
    memcpy(full_sector, "world", 5U);
    if (!liteos_block_cache_invalidate(&fat.Cache, HELLO_DATA_LBA)) return 5;
    hello_data_reads = read_counts[HELLO_DATA_LBA];
    file.Position = 0U;
    if (!liteos_vfs_write(&file, full_sector, sizeof(full_sector), &size) ||
        size != sizeof(full_sector) || read_counts[HELLO_DATA_LBA] != hello_data_reads) return 5;
    if (!liteos_block_cache_invalidate(&fat.Cache, HELLO_DATA_LBA)) return 5;
    hello_data_reads = read_counts[HELLO_DATA_LBA];
    file.Position = 510U;
    if (!liteos_vfs_write(&file, "xy", 2U, &size) || size != 2U ||
        read_counts[HELLO_DATA_LBA] != hello_data_reads + 1U ||
        !liteos_block_cache_read(&fat.Cache, HELLO_DATA_LBA, full_sector) ||
        memcmp(full_sector, "world", 5U) != 0 || full_sector[509U] != 0xA5U ||
        full_sector[510U] != 'x' || full_sector[511U] != 'y') return 5;
    file.Position = 5U;
    if (!liteos_vfs_write(&file, extension, sizeof(extension), &size) ||
        size != sizeof(extension) || !liteos_vfs_close(&file) ||
        !liteos_fat32_sync(&fat) ||
        memcmp(disk + 5U * 512U, extension + 507U, 93U) != 0 ||
        disk[3U * 512U + 28U] != (UINT8)(5U + sizeof(extension)) ||
        disk[512U + 16U] != 0xF8U || disk[1024U + 16U] != 0xF8U ||
        !liteos_fat32_create_path(&fat, "new.bin", 0) ||
        liteos_fat32_create_path(&fat, "new.bin", 0) ||
        !liteos_fat32_create_path(&fat, "Long Created File.txt", 0) ||
        !liteos_vfs_open_access(&vfs, "/Long Created File.txt",
                                LITEOS_ACCESS_READ | LITEOS_ACCESS_WRITE, &file) ||
        !liteos_vfs_write(&file, "lfn", 3U, &size) || size != 3U ||
        !liteos_vfs_close(&file) || !liteos_fat32_sync(&fat)) return 5;
    /* 注入第二次目录写失败，验证短名已删除时 LFN 失败会恢复原目录项。 */
    fat.Cache.Initialized = 0U;
    BOOLEAN before_failure = liteos_fat32_lookup(&fat, "Long Created File.txt", &rollback_node);
    if (!before_failure) return 5;
    if (rollback_node.Operations == 0 || !rollback_node.Operations->Close(&rollback_node)) {
        return 5;
    }
    fail_write_number = 2U;
    write_count = 0U;
    BOOLEAN removed_after_failure = liteos_fat32_remove_path(&fat, "Long Created File.txt");
    BOOLEAN found_after_failure = liteos_fat32_lookup(&fat, "Long Created File.txt", &rollback_node);
    if (removed_after_failure || !found_after_failure) return 5;
    if (rollback_node.Operations == 0 || !rollback_node.Operations->Close(&rollback_node)) {
        return 5;
    }
    fail_write_number = 0U;
    fat.Cache.Initialized = 1U;
    if (!liteos_fat32_remove_path(&fat, "Long Created File.txt") ||
        !liteos_fat32_create_path(&fat, "ninechars.txt", 0) ||
        !liteos_vfs_open_access(&vfs, "/ninechars.txt",
                                LITEOS_ACCESS_READ | LITEOS_ACCESS_WRITE, &file) ||
        !liteos_vfs_write(&file, "chain", 5U, &size) || size != 5U ||
        !liteos_vfs_close(&file) || !liteos_fat32_sync(&fat)) return 5;
    /* 目录短名和 LFN 各写一次，随后在 FAT 副本写入阶段注入失败。 */
    fat.Cache.Initialized = 0U;
    fail_write_number = 4U;
    write_count = 0U;
    BOOLEAN removed_during_fat_failure =
        liteos_fat32_remove_path(&fat, "ninechars.txt");
    BOOLEAN found_during_fat_failure =
        liteos_fat32_lookup(&fat, "ninechars.txt", &rollback_node);
    if (removed_during_fat_failure || !found_during_fat_failure) return 5;
    if (rollback_node.Operations == 0 || !rollback_node.Operations->Close(&rollback_node)) {
        return 5;
    }
    fail_write_number = 0U;
    fat.Cache.Initialized = 1U;
    if (!liteos_fat32_remove_path(&fat, "ninechars.txt") ||
        !liteos_fat32_create_path(&fat, "Long Folder Name", 1) ||
        !liteos_fat32_create_path(&fat, "Long Folder Name/Long Child Name.txt", 0) ||
        !liteos_fat32_remove_path(&fat, "Long Folder Name/Long Child Name.txt") ||
        !liteos_fat32_remove_path(&fat, "Long Folder Name") ||
        !liteos_fat32_create_path(&fat, "folder", 1) ||
        !liteos_fat32_create_path(&fat, "folder/child.bin", 0) ||
        liteos_fat32_remove_path(&fat, "folder") ||
        !liteos_fat32_remove_path(&fat, "folder/child.bin") ||
        !liteos_fat32_remove_path(&fat, "folder") ||
        !liteos_fat32_remove_path(&fat, "new.bin")) return 5;
    if (!liteos_vfs_unmount(&vfs, "/") || !liteos_fat32_destroy(&fat) ||
        memcmp(disk + 4U * 512U, "world", 5U) != 0 ||
        !liteos_block_unregister(&blocks, device) || !liteos_block_manager_destroy(&blocks)) return 6;
    puts("fat32: ok");
    return 0;
}
