#include <kernel/init_filesystem.h>
#include <kernel/bootinfo.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/paging.h>
#include <kernel/block.h>
#include <kernel/debug_stage.h>
#include <kernel/device.h>
#include <kernel/elf_loader.h>
#include <kernel/io.h>
#include <kernel/journal.h>
#include <kernel/litefs.h>
#include <kernel/mm.h>
#include <kernel/vfs.h>
#include <kernel/vm.h>
#include <kernel/block_device.h>
#include <kernel/fat32.h>
#include <kernel/console.h>
#include "filesystem_internal.h"

/* REFACTOR_P3_FILESYSTEM_SELF_TEST_OWNER: FAT32, VFS, and file-map tests. */

#define FAT32_TEST_SECTOR_COUNT 128U
static UINT8 g_fat32_test_disk[FAT32_TEST_SECTOR_COUNT * 512U];

static VOID store_u16(UINT8 *destination, UINT16 value) {
    destination[0] = (UINT8)value;
    destination[1] = (UINT8)(value >> 8);
}

static VOID store_u32(UINT8 *destination, UINT32 value) {
    destination[0] = (UINT8)value;
    destination[1] = (UINT8)(value >> 8);
    destination[2] = (UINT8)(value >> 16);
    destination[3] = (UINT8)(value >> 24);
}

static BOOLEAN fat32_test_read(VOID *context, UINT64 lba, UINT32 count, VOID *buffer) {
    UINT8 *disk = (UINT8 *)context;
    if (disk == 0 || buffer == 0 || lba >= FAT32_TEST_SECTOR_COUNT ||
        count > FAT32_TEST_SECTOR_COUNT - lba) return 0;
    for (UINT32 i = 0; i < count * 512U; ++i) {
        ((UINT8 *)buffer)[i] = disk[lba * 512ULL + i];
    }
    return 1;
}

static BOOLEAN fat32_test_write(VOID *context, UINT64 lba, UINT32 count,
                                const VOID *buffer) {
    UINT8 *disk = (UINT8 *)context;
    if (disk == 0 || buffer == 0 || lba >= FAT32_TEST_SECTOR_COUNT ||
        count > FAT32_TEST_SECTOR_COUNT - lba) return 0;
    for (UINT32 i = 0; i < count * 512U; ++i) {
        disk[lba * 512ULL + i] = ((const UINT8 *)buffer)[i];
    }
    return 1;
}

static BOOLEAN filesystem_vfs_fat_delete_self_test(void) {
    static const CHAR8 path[] = "/vfsdel.asm";
    static const CHAR8 fileman_path[] = "/VFSDEL.ASM";
    static const CHAR8 source[] = "bits 64\n";
    file_t *file = 0;
    os_file_info_t info = {0};
    UINT64 bytes = 0U;
    CHAR8 result[sizeof(source)] = {0};
    BOOLEAN success = 0;

    if (vfs_open_kernel(path,
                        VFS_OPEN_READ | VFS_OPEN_WRITE | VFS_OPEN_CREATE |
                            VFS_OPEN_TRUNCATE,
                        0666U, &file) != K_OK || file == 0 ||
        vfs_write_kernel(file, source, sizeof(source) - 1U, &bytes) != K_OK ||
        bytes != sizeof(source) - 1U || vfs_remove_kernel(fileman_path) != K_OK ||
        vfs_seek(file, 0, OS_FILE_SEEK_SET, &bytes) != K_OK ||
        vfs_read_kernel(file, result, sizeof(source) - 1U, &bytes) != K_OK ||
        bytes != sizeof(source) - 1U || result[0] != 'b') {
        goto cleanup;
    }
    vfs_close(file);
    file = 0;

    if (vfs_stat_kernel(path, &info) != K_ENOENT ||
        vfs_open_kernel(path, VFS_OPEN_READ, 0U, &file) != K_ENOENT) {
        goto cleanup;
    }
    if (file != 0) goto cleanup;
    if (vfs_open_kernel(path,
                        VFS_OPEN_READ | VFS_OPEN_WRITE | VFS_OPEN_CREATE |
                            VFS_OPEN_TRUNCATE,
                        0666U, &file) != K_OK || file == 0 ||
        vfs_remove_kernel(fileman_path) != K_OK ||
        vfs_stat_kernel(path, &info) != K_ENOENT) {
        goto cleanup;
    }
    vfs_close(file);
    file = 0;
    success = 1;

cleanup:
    if (file != 0) vfs_close(file);
    if (!success) (void)vfs_remove_kernel(path);
    return success;
}

BOOLEAN filesystem_vfs_file_api_self_test(void) {
    file_t *file = 0;
    os_file_info_t info = {0};
    uint64_t bytes = 0U;
    uint64_t position = 0U;
    char result[4] = {0};
    bool success = false;
    kstatus_t status = vfs_mkdir_kernel("/vfs-api", 0755U);
    if (status != K_OK) {
        liteos_serial_write("LITEOS_VFS_API_FAIL_STAGE=1 STATUS=");
        liteos_serial_write_u32((uint32_t)(-status));
        liteos_serial_write("\r\n");
        goto cleanup;
    }
    if (
        vfs_open_kernel("/vfs-api/file", VFS_OPEN_READ | VFS_OPEN_WRITE |
                        VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE, 0666U,
                        &file) != K_OK ||
        vfs_write_kernel(file, "abc", 3U, &bytes) != K_OK || bytes != 3U ||
        vfs_seek(file, 0, OS_FILE_SEEK_SET, &position) != K_OK || position != 0U ||
        vfs_read_kernel(file, result, 3U, &bytes) != K_OK || bytes != 3U ||
        result[0] != 'a' || result[1] != 'b' || result[2] != 'c' ||
        vfs_truncate_kernel(file, 1U) != K_OK ||
        vfs_stat_kernel("/vfs-api/file", &info) != K_OK || info.size != 1U ||
        info.type != OS_FILE_TYPE_REGULAR ||
        vfs_rename_kernel("/vfs-api/file", "/vfs-api/renamed") != K_OK ||
        vfs_stat_kernel("/vfs-api/file", &info) != K_ENOENT ||
        vfs_stat_kernel("/vfs-api/renamed", &info) != K_OK ||
        info.size != 1U || info.type != OS_FILE_TYPE_REGULAR ||
        vfs_seek(file, 0, OS_FILE_SEEK_SET, &position) != K_OK ||
        vfs_read_kernel(file, result, 1U, &bytes) != K_OK || bytes != 1U ||
        result[0] != 'a') goto cleanup;
    vfs_close(file);
    file = 0;
    if (vfs_stat_kernel("/vfs-api", &info) != K_OK ||
        info.type != OS_FILE_TYPE_DIRECTORY ||
        vfs_remove_kernel("/vfs-api/renamed") != K_OK ||
        vfs_remove_kernel("/vfs-api") != K_OK) goto cleanup;
    success = true;
cleanup:
    if (file != 0) vfs_close(file);
    if (!success) {
        (void)vfs_remove_kernel("/vfs-api/file");
        (void)vfs_remove_kernel("/vfs-api/renamed");
        (void)vfs_remove_kernel("/vfs-api");
    }
    return success;
}

BOOLEAN filesystem_fat32_self_test(void) {
    static LITEOS_BLOCK_MANAGER block_manager;
    static LITEOS_BLOCK_DEVICE *device;
    static LITEOS_FAT32 filesystem;
    CHAR8 buffer[6] = {0};
    UINT32 size = 0;
    LITEOS_FAT32_FILE *held_file = 0;
    UINT8 *fat1;
    UINT8 *fat2;
    UINT8 *root;
    UINT8 *data;
    for (UINT32 i = 0; i < sizeof(g_fat32_test_disk); ++i) g_fat32_test_disk[i] = 0;
    g_fat32_test_disk[510] = 0x55U;
    g_fat32_test_disk[511] = 0xAAU;
    store_u16(g_fat32_test_disk + 11U, 512U);
    g_fat32_test_disk[13] = 1U;
    store_u16(g_fat32_test_disk + 14U, 1U);
    g_fat32_test_disk[16] = 2U;
    store_u16(g_fat32_test_disk + 19U, FAT32_TEST_SECTOR_COUNT);
    store_u32(g_fat32_test_disk + 36U, 1U);
    store_u32(g_fat32_test_disk + 44U, 2U);
    fat1 = g_fat32_test_disk + 512U;
    fat2 = fat1 + 512U;
    store_u32(fat1 + 0U, 0x0FFFFFF8U);
    store_u32(fat1 + 4U, 0xFFFFFFFFU);
    store_u32(fat1 + 8U, 0x0FFFFFFFU);
    store_u32(fat1 + 12U, 0x0FFFFFFFU);
    for (UINT32 i = 0; i < 512U; ++i) fat2[i] = fat1[i];
    root = g_fat32_test_disk + 3U * 512U;
    for (UINT32 i = 0; i < 11U; ++i) root[i] = (UINT8)"SAMPLE  TXT"[i];
    root[11] = 0x20U;
    store_u16(root + 26U, 3U);
    store_u32(root + 28U, 5U);
    data = g_fat32_test_disk + 4U * 512U;
    data[0] = 'h'; data[1] = 'e'; data[2] = 'l'; data[3] = 'l'; data[4] = 'o';
    if (!liteos_block_manager_init(&block_manager)) return 0;
    if (!liteos_block_register(&block_manager, "mem0", 512U, FAT32_TEST_SECTOR_COUNT,
                               fat32_test_read, fat32_test_write, 0,
                               g_fat32_test_disk, &device)) return 0;
    if (!liteos_fat32_init(&filesystem, device)) return 0;
    if (!liteos_fat32_read_path(&filesystem, "sample.txt", 0U, buffer, 5U, &size) ||
        size != 5U ||
        buffer[0] != 'h' || buffer[4] != 'o') return 0;
    if (!liteos_fat32_write_path(&filesystem, "sample.txt", 0U,
                                 "world", 5U, &size) || size != 5U ||
        !liteos_fat32_create_path(&filesystem, "new.txt", 0) ||
        liteos_fat32_create_path(&filesystem, "new.txt", 0) ||
        !liteos_fat32_create_path(&filesystem, "Long Created File.txt", 0) ||
        !liteos_fat32_write_path(&filesystem, "Long Created File.txt", 0U,
                                 "lfn", 3U, &size) || size != 3U ||
        !liteos_fat32_remove_path(&filesystem, "Long Created File.txt") ||
        !liteos_fat32_create_path(&filesystem, "Long Created File.txt", 0) ||
        !liteos_fat32_open(&filesystem, "Long Created File.txt", &held_file) ||
        liteos_fat32_remove_path(&filesystem, "Long Created File.txt") ||
        !liteos_fat32_close(held_file) ||
        !liteos_fat32_remove_path(&filesystem, "Long Created File.txt") ||
        !liteos_fat32_create_path(&filesystem, "Long Folder Name", 1) ||
        !liteos_fat32_create_path(&filesystem, "Long Folder Name/Long Child Name.txt", 0) ||
        !liteos_fat32_remove_path(&filesystem, "Long Folder Name/Long Child Name.txt") ||
        !liteos_fat32_remove_path(&filesystem, "Long Folder Name") ||
        !liteos_fat32_create_path(&filesystem, "subdir", 1) ||
        !liteos_fat32_create_path(&filesystem, "subdir/inner.txt", 0) ||
        liteos_fat32_remove_path(&filesystem, "subdir") ||
        !liteos_fat32_remove_path(&filesystem, "subdir/inner.txt") ||
        !liteos_fat32_remove_path(&filesystem, "subdir") ||
        !liteos_fat32_remove_path(&filesystem, "new.txt") ||
        !liteos_fat32_sync(&filesystem) ||
        g_fat32_test_disk[4U * 512U] != 'w' ||
        g_fat32_test_disk[4U * 512U + 4U] != 'd' ||
        vfs_mount_fat32("/", &filesystem) != K_OK) return 0;
    os_file_info_t mounted_info = {0};
    if (vfs_stat_kernel("/sample.txt", &mounted_info) != K_OK ||
        mounted_info.type != OS_FILE_TYPE_REGULAR || mounted_info.size != 5U ||
        vfs_enumerate_kernel("/", 0U, &mounted_info) != K_OK ||
        mounted_info.type != OS_FILE_TYPE_REGULAR) {
        (void)vfs_unmount_fat32();
        return 0;
    }
    if (!filesystem_vfs_fat_delete_self_test()) {
        (void)vfs_unmount_fat32();
        return 0;
    }
    if (vfs_unmount_fat32() != K_OK) return 0;
    return 1;
}

/* 验证同一个 vnode 的文件页在多个地址空间之间共享统一页缓存。 */
BOOLEAN filesystem_file_mapping_self_test(void) {
    /*
     * The boot image pre-seeds this 8.3 file.  QEMU 10's vvfat backend
     * asserts when a guest creates/removes a directory entry, so the test
     * reuses the existing inode and only exercises vnode/page-cache mapping.
     */
    static const CHAR8 test_path[] = "/etc/vfsmap.tst";
    static const UINT8 initial[] = {'w', 'o', 'r', 'l', 'd'};
    file_t *file = 0;
    file_t *write_file = 0;
    vm_object_t *object = 0;
    vm_space_t *first = 0;
    vm_space_t *second = 0;
    vm_space_t *shared = 0;
    vm_space_t *private_space = 0;
    vm_space_t *private_child = 0;
    vaddr_t first_address = 0x0000000050000000ULL;
    vaddr_t second_address = 0x0000000060000000ULL;
    vaddr_t shared_address = 0x0000000070000000ULL;
    vaddr_t private_address = 0x0000000080000000ULL;
    paddr_t first_physical = paddr_make(0);
    paddr_t second_physical = paddr_make(0);
    paddr_t shared_physical = paddr_make(0);
    paddr_t private_physical = paddr_make(0);
    paddr_t private_child_physical = paddr_make(0);
    UINT32 failure_stage = 0;
    UINT64 initial_written = 0;
    BOOLEAN success = 0;
    if (vfs_open_kernel(test_path,
                        VFS_OPEN_READ | VFS_OPEN_WRITE, 0U, &file) != K_OK ||
        file == 0) {
        failure_stage = 101;
        goto cleanup;
    }
    kstatus_t write_status = vfs_write_kernel(file, initial, sizeof(initial),
                                              &initial_written);
    if (write_status != K_OK || initial_written != sizeof(initial)) {
        failure_stage = 103;
        goto cleanup;
    }
    file->position = 0;
    if (vm_object_create_file(file->vnode, vfs_vm_file_ops(),
                              file->vnode->size, 0, PAGE_SIZE, &object) != K_OK) {
        failure_stage = 2;
        goto cleanup;
    }
    if (vm_space_create(&first) != K_OK || vm_space_create(&second) != K_OK ||
        vm_space_create(&shared) != K_OK || vm_space_create(&private_space) != K_OK) {
        failure_stage = 3;
        goto cleanup;
    }
    if (vm_map_object(first, object, &first_address, 0, PAGE_SIZE,
                      VM_PROT_READ | VM_PROT_USER,
                      VM_MAP_PRIVATE | VM_MAP_FIXED) != K_OK ||
        vm_map_object(second, object, &second_address, 0, PAGE_SIZE,
                      VM_PROT_READ | VM_PROT_USER,
                      VM_MAP_PRIVATE | VM_MAP_FIXED) != K_OK ||
        vm_map_object(shared, object, &shared_address, 0, PAGE_SIZE,
                      VM_PROT_READ | VM_PROT_WRITE | VM_PROT_USER,
                      VM_MAP_SHARED | VM_MAP_FIXED) != K_OK) {
        failure_stage = 4;
        goto cleanup;
    }
    if (vm_handle_fault(first, &(vm_fault_info_t){first_address, VM_PROT_READ, 0}) != K_OK ||
        vm_handle_fault(second, &(vm_fault_info_t){second_address, VM_PROT_READ, 0}) != K_OK ||
        vm_handle_fault(shared, &(vm_fault_info_t){shared_address, VM_PROT_WRITE, 0}) != K_OK) {
        failure_stage = 5;
        goto cleanup;
    }
    if (x86_translate_page(first->root_table, first_address, &first_physical, 0) != K_OK ||
        x86_translate_page(second->root_table, second_address, &second_physical, 0) != K_OK ||
        x86_translate_page(shared->root_table, shared_address, &shared_physical, 0) != K_OK) {
        failure_stage = 6;
        goto cleanup;
    }
    if (first_physical.value != second_physical.value ||
        first_physical.value != shared_physical.value) {
        failure_stage = 7;
        goto cleanup;
    }
    UINT8 *bytes = (UINT8 *)phys_to_direct(first_physical);
    success = bytes != 0 && bytes[0] == 'w' && bytes[1] == 'o' &&
              bytes[2] == 'r' && bytes[3] == 'l' && bytes[4] == 'd';
    if (!success) failure_stage = 8;
    if (success &&
        (vm_map_object(private_space, object, &private_address, 0, PAGE_SIZE,
                       VM_PROT_READ | VM_PROT_WRITE | VM_PROT_USER,
                       VM_MAP_PRIVATE | VM_MAP_FIXED) != K_OK ||
         vm_handle_fault(private_space,
                         &(vm_fault_info_t){private_address, VM_PROT_READ, 0}) != K_OK ||
         vm_handle_fault(private_space,
                         &(vm_fault_info_t){private_address, VM_PROT_WRITE, 0}) != K_OK ||
         x86_translate_page(private_space->root_table, private_address,
                            &private_physical, 0) != K_OK)) {
        failure_stage = 14;
        success = false;
    }
    if (success) {
        UINT8 *private_bytes = (UINT8 *)phys_to_direct(private_physical);
        if (private_physical.value == shared_physical.value || private_bytes == 0 ||
            private_bytes[0] != 'w' || private_bytes[4] != 'd') {
            failure_stage = 15;
            success = false;
        } else {
            /* 私有映射写入只修改 shadow，文件页和共享映射仍保持原内容。 */
            private_bytes[0] = 'X';
            if (vm_space_clone_cow(private_space, &private_child) != K_OK ||
                vm_handle_fault(private_child,
                                &(vm_fault_info_t){private_address, VM_PROT_READ, 0}) != K_OK ||
                x86_translate_page(private_child->root_table, private_address,
                                   &private_child_physical, 0) != K_OK ||
                private_child_physical.value != private_physical.value ||
                vm_handle_fault(private_child,
                                &(vm_fault_info_t){private_address, VM_PROT_WRITE, 3U}) != K_OK ||
                x86_translate_page(private_child->root_table, private_address,
                                   &private_child_physical, 0) != K_OK ||
                private_child_physical.value == private_physical.value) {
                failure_stage = 16;
                success = false;
            } else {
                UINT8 *child_bytes = (UINT8 *)phys_to_direct(private_child_physical);
                if (child_bytes == 0 || child_bytes[0] != 'X') {
                    failure_stage = 17;
                    success = false;
                } else {
                    child_bytes[0] = 'Y';
                }
            }
            if (success && (private_bytes[0] != 'X' || bytes[0] != 'w')) {
                failure_stage = 18;
                success = false;
            }
        }
    }
    if (success) {
        static const UINT8 upper[] = {'W', 'O', 'R', 'L', 'D'};
        static const UINT8 lower[] = {'w', 'o', 'r', 'l', 'd'};
        UINT64 written = 0;
        if (vfs_open_kernel(test_path,
                            VFS_OPEN_READ | VFS_OPEN_WRITE,
                            0U, &write_file) != K_OK) {
            failure_stage = 8;
            success = false;
        } else if (vfs_write_kernel(write_file, upper, sizeof(upper), &written) != K_OK ||
                   written != sizeof(upper) || bytes[0] != 'W' || bytes[4] != 'D') {
            failure_stage = 9;
            success = false;
        } else {
            write_file->position = 0;
            if (vfs_write_kernel(write_file, lower, sizeof(lower), &written) != K_OK ||
                written != sizeof(lower)) {
                failure_stage = 10;
                success = false;
            } else {
                bytes[0] = 'm';
                bytes[1] = 'm';
                bytes[2] = 'a';
                bytes[3] = 'p';
                bytes[4] = '!';
                /* 模拟页回收器从硬件 PTE 脏位发现该共享映射已被修改。 */
                vfs_file_page_mark_dirty(write_file->vnode, 0);
                kstatus_t sync_status = vfs_fsync(write_file);
                if (sync_status != K_OK) {
                    failure_stage = 11;
                    liteos_serial_write("LITEOS_VFS_FILEMAP_DEBUG_STATUS=");
                    liteos_serial_write_u32((UINT32)(-sync_status));
                    liteos_serial_write("\r\n");
                    success = false;
                } else {
                    write_file->position = 0;
                    if (vfs_write_kernel(write_file, lower, sizeof(lower), &written) != K_OK ||
                        written != sizeof(lower) || vfs_fsync(write_file) != K_OK) {
                        failure_stage = 12;
                        success = false;
                    }
                }
            }
        }
    }

cleanup:
    if (!success && failure_stage != 0) {
        liteos_serial_write("LITEOS_VFS_FILEMAP_DEBUG_STAGE=");
        liteos_serial_write_u32(failure_stage);
        liteos_serial_write("\r\n");
    }
    if (write_file != 0) vfs_close(write_file);
    if (private_child != 0) vm_space_put(private_child);
    if (private_space != 0) vm_space_put(private_space);
    if (shared != 0) vm_space_put(shared);
    if (second != 0) vm_space_put(second);
    if (first != 0) vm_space_put(first);
    if (object != 0) vm_object_put(object);
    if (file != 0) vfs_close(file);
    return success;
}
