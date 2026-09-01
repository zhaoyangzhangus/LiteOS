#include <kernel/init_storage.h>

#include <kernel/debug_stage.h>

static BOOLEAN storage_fail_at(const liteos_init_storage_hooks_t *hooks,
                               const CHAR8 *message, const char *file,
                               uint32_t line) {
    liteos_debug_stage_fail_at(LITEOS_DEBUG_PHASE_STORAGE,
                               LITEOS_DEBUG_STEP_FAIL, K_EIO, file, line);
    hooks->write(message);
    hooks->halt();
    return 0;
}

#define storage_fail(hooks, message) \
    storage_fail_at((hooks), (message), __FILE__, __LINE__)

BOOLEAN liteos_init_storage(const liteos_init_storage_hooks_t *hooks,
                            const nvme_controller_t **active_controller) {
    if (hooks == 0 || hooks->write == 0 || hooks->write_u32 == 0 ||
        hooks->halt == 0 || active_controller == 0) return 0;

    *active_controller = 0;
    liteos_debug_stage_enter(LITEOS_DEBUG_PHASE_SPEC_4);
    if (!nvme_driver_self_test()) {
        hooks->write("LITEOS_NVME_CORE_FAIL=");
        hooks->write_u32((UINT32)(-nvme_last_error()));
        hooks->write(" STAGE=");
        hooks->write_u32(nvme_last_stage());
        hooks->write(" STATUS=");
        hooks->write_u32(nvme_last_completion_status());
        hooks->write(" HARDWARE=");
        hooks->write_u32(nvme_hardware_present() ? 1U : 0U);
        return storage_fail(hooks, "\r\n");
    }
    hooks->write("LITEOS_NVME_CORE_OK\r\n");
    *active_controller = nvme_active_controller();
    if (*active_controller != 0) {
        hooks->write("LITEOS_NVME_HW_OK\r\nLITEOS_NVME_NAMESPACE_COUNT=");
        hooks->write_u32((*active_controller)->namespace_count);
        hooks->write("\r\nLITEOS_NVME_IO_QUEUE_COUNT=");
        hooks->write_u32((*active_controller)->io_queue_count);
        hooks->write("\r\n");
    }
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_3,
                       LITEOS_DEBUG_STEP_PROGRESS, 5U);
    return 1;
}
