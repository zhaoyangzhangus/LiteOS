#include <kernel/init_filesystem.h>

#include <kernel/debug_stage.h>
#include <kernel/elf_loader.h>
#include <kernel/journal.h>
#include <kernel/litefs.h>
#include <kernel/realtest.h>
#include "filesystem_internal.h"
#include "filesystem_root_internal.h"

#ifndef OS_REALTEST_FAILURE_TEST
#define OS_REALTEST_FAILURE_TEST 0
#endif

static BOOLEAN g_fat32_self_tested;

static BOOLEAN filesystem_fail_at(const kernel_init_filesystem_hooks_t *hooks,
                                  const CHAR8 *message, const char *file,
                                  uint32_t line) {
    kernel_debug_stage_fail_at(OS_DEBUG_PHASE_STORAGE,
                               OS_DEBUG_STEP_FAIL, K_EIO, file, line);
    hooks->write(message);
    hooks->halt();
    return 0;
}

#define filesystem_fail(hooks, message) \
    filesystem_fail_at((hooks), (message), __FILE__, __LINE__)

BOOLEAN prepare_realtest_root(const OS_BOOT_INFO *boot_info) {
    realtest_mark("ROOT_PROBE_BEGIN");
    if (!g_fat32_self_tested) {
        g_fat32_self_tested = filesystem_fat32_self_test();
        if (!g_fat32_self_tested) {
            realtest_mark("ROOT_FAT_SELFTEST_FAIL");
            return 0;
        }
    }
    if (!filesystem_mount_usb_root(boot_info)) {
        realtest_mark("ROOT_PROBE_FAIL");
        return 0;
    }
    realtest_mark("ROOT_PROBE_OK");
    realtest_filesystem_ready();
    return 1;
}

BOOLEAN kernel_init_filesystem(const OS_BOOT_INFO *boot_info,
                               const nvme_controller_t *active_controller,
                               const kernel_init_filesystem_hooks_t *hooks) {
    BOOLEAN root_mounted = 0;
    BOOLEAN root_is_nvme = 0;
    if (hooks == 0 || hooks->write == 0 || hooks->write_u32 == 0 ||
        hooks->halt == 0) return 0;

    if (!g_fat32_self_tested &&
        !(g_fat32_self_tested = filesystem_fat32_self_test())) {
        return filesystem_fail(hooks, "OS_FAT32_TEST_FAIL\r\n");
    }
    hooks->write("OS_FAT32_OK\r\n");

    if (filesystem_mount_all_volumes(boot_info, active_controller,
                                     &root_is_nvme)) {
        root_mounted = 1;
        hooks->write(root_is_nvme ? "OS_ROOT_NVME_OK\r\n" :
                     "OS_ROOT_USB_OK\r\n");
        hooks->write(root_is_nvme ? "OS_ROOT_SOURCE=NVME\r\n" :
                     "OS_ROOT_SOURCE=USB\r\n");
    }

    if (!root_mounted) {
        return filesystem_fail(hooks, "OS_ROOT_MOUNT_FAIL USB_OR_NVME\r\n");
    }
    realtest_filesystem_ready();
#if OS_REALTEST_FAILURE_TEST
    hooks->write("OS_REALTEST_FORCED_FAILURE\r\n");
    hooks->halt();
#endif

    kernel_debug_stage_enter(OS_DEBUG_PHASE_SPEC_7);
    if (!user_elf_loader_self_test()) {
        return filesystem_fail(hooks, "OS_USER_ELF_FAIL\r\n");
    }
    hooks->write("OS_USER_ELF_OK\r\n");
    if (!filesystem_vfs_file_api_self_test()) {
        return filesystem_fail(hooks, "OS_VFS_FILE_API_FAIL\r\n");
    }
    hooks->write("OS_VFS_OK\r\n");
    hooks->write("OS_VFS_FILE_API_OK\r\n");
    if (!filesystem_file_mapping_self_test()) {
        return filesystem_fail(hooks, "OS_VFS_FILEMAP_FAIL\r\n");
    }
    hooks->write("OS_VFS_FILEMAP_OK\r\n");
    if (!journal_self_test()) {
        return filesystem_fail(hooks, "OS_JOURNAL_TEST_FAIL\r\n");
    }
    hooks->write("OS_JOURNAL_OK\r\n");
    if (active_controller != 0 &&
        !journal_block_storage_self_test(active_controller->device, 1024U, 8U)) {
        return filesystem_fail(hooks, "OS_JOURNAL_BLOCK_IO_FAIL\r\n");
    }
    if (active_controller != 0) hooks->write("OS_JOURNAL_BLOCK_IO_OK\r\n");
    if (!litefs_self_test()) {
        return filesystem_fail(hooks, "OS_LITEFS_FAIL\r\n");
    }
    hooks->write("OS_LITEFS_OK\r\n");
    kernel_debug_stage(OS_DEBUG_PHASE_STORAGE,
                       OS_DEBUG_STEP_READY, 1U);
    kernel_debug_stage_ready(OS_DEBUG_PHASE_SPEC_10);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_3,
                       OS_DEBUG_STEP_PROGRESS, 7U);
    return 1;
}
