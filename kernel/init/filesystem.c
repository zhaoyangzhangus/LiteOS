#include <kernel/init_filesystem.h>

#include <kernel/debug_stage.h>
#include <kernel/elf_loader.h>
#include <kernel/journal.h>
#include <kernel/litefs.h>
#include <kernel/realtest.h>
#include "filesystem_internal.h"
#include "filesystem_root_internal.h"

#ifndef LITEOS_REALTEST_FAILURE_TEST
#define LITEOS_REALTEST_FAILURE_TEST 0
#endif

static BOOLEAN g_fat32_self_tested;

static BOOLEAN filesystem_fail_at(const liteos_init_filesystem_hooks_t *hooks,
                                  const CHAR8 *message, const char *file,
                                  uint32_t line) {
    liteos_debug_stage_fail_at(LITEOS_DEBUG_PHASE_STORAGE,
                               LITEOS_DEBUG_STEP_FAIL, K_EIO, file, line);
    hooks->write(message);
    hooks->halt();
    return 0;
}

#define filesystem_fail(hooks, message) \
    filesystem_fail_at((hooks), (message), __FILE__, __LINE__)

BOOLEAN liteos_prepare_realtest_root(const LITEOS_BOOT_INFO *boot_info) {
    liteos_realtest_mark("ROOT_PROBE_BEGIN");
    if (!g_fat32_self_tested) {
        g_fat32_self_tested = filesystem_fat32_self_test();
        if (!g_fat32_self_tested) {
            liteos_realtest_mark("ROOT_FAT_SELFTEST_FAIL");
            return 0;
        }
    }
    if (!filesystem_mount_usb_root(boot_info)) {
        liteos_realtest_mark("ROOT_PROBE_FAIL");
        return 0;
    }
    liteos_realtest_mark("ROOT_PROBE_OK");
    liteos_realtest_filesystem_ready();
    return 1;
}

BOOLEAN liteos_init_filesystem(const LITEOS_BOOT_INFO *boot_info,
                               const nvme_controller_t *active_controller,
                               const liteos_init_filesystem_hooks_t *hooks) {
    BOOLEAN root_mounted = 0;
    BOOLEAN root_is_nvme = 0;
    if (hooks == 0 || hooks->write == 0 || hooks->write_u32 == 0 ||
        hooks->halt == 0) return 0;

    if (!g_fat32_self_tested &&
        !(g_fat32_self_tested = filesystem_fat32_self_test())) {
        return filesystem_fail(hooks, "LITEOS_FAT32_TEST_FAIL\r\n");
    }
    hooks->write("LITEOS_FAT32_OK\r\n");

    if (filesystem_mount_all_volumes(boot_info, active_controller,
                                     &root_is_nvme)) {
        root_mounted = 1;
        hooks->write(root_is_nvme ? "LITEOS_ROOT_NVME_OK\r\n" :
                     "LITEOS_ROOT_USB_OK\r\n");
        hooks->write(root_is_nvme ? "LITEOS_ROOT_SOURCE=NVME\r\n" :
                     "LITEOS_ROOT_SOURCE=USB\r\n");
    }

    if (!root_mounted) {
        return filesystem_fail(hooks, "LITEOS_ROOT_MOUNT_FAIL USB_OR_NVME\r\n");
    }
    liteos_realtest_filesystem_ready();
#if LITEOS_REALTEST_FAILURE_TEST
    hooks->write("LITEOS_REALTEST_FORCED_FAILURE\r\n");
    hooks->halt();
#endif

    liteos_debug_stage_enter(LITEOS_DEBUG_PHASE_SPEC_7);
    if (!user_elf_loader_self_test()) {
        return filesystem_fail(hooks, "LITEOS_USER_ELF_FAIL\r\n");
    }
    hooks->write("LITEOS_USER_ELF_OK\r\n");
    if (!filesystem_vfs_file_api_self_test()) {
        return filesystem_fail(hooks, "LITEOS_VFS_FILE_API_FAIL\r\n");
    }
    hooks->write("LITEOS_VFS_OK\r\n");
    hooks->write("LITEOS_VFS_FILE_API_OK\r\n");
    if (!filesystem_file_mapping_self_test()) {
        return filesystem_fail(hooks, "LITEOS_VFS_FILEMAP_FAIL\r\n");
    }
    hooks->write("LITEOS_VFS_FILEMAP_OK\r\n");
    if (!journal_self_test()) {
        return filesystem_fail(hooks, "LITEOS_JOURNAL_TEST_FAIL\r\n");
    }
    hooks->write("LITEOS_JOURNAL_OK\r\n");
    if (active_controller != 0 &&
        !journal_block_storage_self_test(active_controller->device, 1024U, 8U)) {
        return filesystem_fail(hooks, "LITEOS_JOURNAL_BLOCK_IO_FAIL\r\n");
    }
    if (active_controller != 0) hooks->write("LITEOS_JOURNAL_BLOCK_IO_OK\r\n");
    if (!litefs_self_test()) {
        return filesystem_fail(hooks, "LITEOS_LITEFS_FAIL\r\n");
    }
    hooks->write("LITEOS_LITEFS_OK\r\n");
    liteos_debug_stage(LITEOS_DEBUG_PHASE_STORAGE,
                       LITEOS_DEBUG_STEP_READY, 1U);
    liteos_debug_stage_ready(LITEOS_DEBUG_PHASE_SPEC_10);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_3,
                       LITEOS_DEBUG_STEP_PROGRESS, 7U);
    return 1;
}
