#include <kernel/bootinfo.h>
#include <kernel/console.h>
#include <kernel/console_backend.h>
#include <kernel/debug_stage.h>
#include <kernel/init_core.h>
#include <kernel/init_devices.h>
#include <kernel/init_early.h>
#include <kernel/init_filesystem.h>
#include <kernel/init_memory.h>
#include <kernel/init_runtime.h>
#include <kernel/init_scheduler.h>
#include <kernel/init_self_tests.h>
#include <kernel/init_storage.h>
#include <kernel/nvme_core.h>
#include <kernel/realtest.h>

static void __attribute__((noreturn)) kernel_halt(void) {
    for (;;) __asm__ volatile ("hlt");
}

void __attribute__((noreturn)) liteos_kernel_halt_forever(void) {
    liteos_realtest_record_failure(liteos_debug_stage_file(),
                                   liteos_debug_stage_line());
    (void)liteos_realtest_flush();
    if (liteos_realtest_enabled()) liteos_realtest_finish_failure();
    kernel_halt();
}

/* Keep the direct boot-entry failure tied to an exact source location. */
void __attribute__((noreturn)) liteos_kernel_halt_forever_at(const char *file,
                                                            uint32_t line) {
    liteos_realtest_record_failure(file, line);
    uint16_t phase = liteos_debug_stage_phase();
    if (phase == 0U) phase = LITEOS_DEBUG_PHASE_BOOT;
    liteos_debug_stage_fail_at(phase, LITEOS_DEBUG_STEP_FAIL, K_EIO,
                               file, line);
    (void)liteos_realtest_flush();
    if (liteos_realtest_enabled()) liteos_realtest_finish_failure();
    kernel_halt();
}

#define halt_forever() liteos_kernel_halt_forever_at(__FILE__, __LINE__)

/*
 * This function only orders the named initialization Owners.  Post-scheduler
 * self-tests and the high-half continuation live behind explicit boundaries
 * so a debugger can stop at the transition that owns the failure.
 */
void kernel_entry(LITEOS_BOOT_INFO *info) {
    UINT64 framebuffer_virtual_base = 0U;

    liteos_realtest_boot_info(info);
    liteos_realtest_mark("KERNEL_ENTRY");
    liteos_console_serial_init();
    liteos_serial_write_serial_only("LITEOS_KERNEL_ENTRY\r\n");
    if (liteos_console_init_early(info)) {
        liteos_serial_write_serial_only("LITEOS_EARLY_GOP_OK\r\n");
    }

    liteos_init_early_hooks_t early_hooks = {
        .write = liteos_serial_write_serial_only,
        .halt = liteos_kernel_halt_forever,
    };
    if (!liteos_init_early(info, &early_hooks)) halt_forever();
    liteos_realtest_mark("EARLY_OK");

    liteos_init_memory_hooks_t memory_hooks = {
        .write = liteos_serial_write_serial_only,
        .halt = liteos_kernel_halt_forever,
        .console_init = liteos_console_init,
        .console_disable = liteos_console_disable,
    };
    if (!liteos_init_memory(info, &memory_hooks,
                            &framebuffer_virtual_base)) halt_forever();
    liteos_realtest_mark("MEMORY_OK");

    liteos_init_devices_hooks_t device_hooks = {
        .write = liteos_serial_write_serial_only,
        .write_u32 = liteos_serial_write_u32_serial_only,
        .halt = liteos_kernel_halt_forever,
    };
    if (!liteos_init_devices(info, &device_hooks)) halt_forever();
    liteos_realtest_mark("DEVICES_OK");

    liteos_init_core_hooks_t core_hooks = {
        .write = liteos_serial_write_serial_only,
        .halt = liteos_kernel_halt_forever,
    };
    if (!liteos_init_core(&core_hooks)) halt_forever();
    liteos_realtest_mark("CORE_OK");

#if LITEOS_REALTEST
    if (liteos_prepare_realtest_root(info)) {
        liteos_serial_write_serial_only("LITEOS_REALTEST_EARLY_ROOT_OK\r\n");
        liteos_realtest_checkpoint("ROOT_PROBE_READY");
    } else {
        liteos_serial_write_serial_only("LITEOS_REALTEST_EARLY_ROOT_FAIL\r\n");
        /* A real-hardware result is only valid when its trace is on the test
         * USB volume. Do not silently continue on the NVMe fallback, because
         * that would make the host copy the loader-only log from F:. */
        liteos_realtest_mark("FAILURE_ROOT_PROBE");
        liteos_realtest_finish_failure();
    }
#endif

    const nvme_controller_t *active_controller = 0;
    liteos_realtest_checkpoint("STORAGE_BEGIN");
    liteos_init_storage_hooks_t storage_hooks = {
        .write = liteos_serial_write_serial_only,
        .write_u32 = liteos_serial_write_u32_serial_only,
        .halt = liteos_kernel_halt_forever,
    };
    if (!liteos_init_storage(&storage_hooks, &active_controller)) {
        halt_forever();
    }
    liteos_realtest_mark("STORAGE_OK");
    liteos_realtest_checkpoint("STORAGE_OK");

    liteos_init_scheduler_hooks_t scheduler_hooks = {
        .write = liteos_serial_write_serial_only,
        .write_u32 = liteos_serial_write_u32_serial_only,
        .halt = liteos_kernel_halt_forever,
    };
    liteos_realtest_mark("SCHEDULER_BEGIN");
    liteos_realtest_checkpoint("SCHEDULER_BEGIN");
    if (!liteos_init_scheduler(info, &scheduler_hooks)) halt_forever();
    liteos_realtest_mark("SCHEDULER_OK");
    liteos_realtest_checkpoint("SCHEDULER_OK");

    liteos_realtest_mark("POST_SCHEDULER_BEGIN");
    /* From this boundary onward the framebuffer belongs to the desktop.
     * Diagnostics remain in the realtest capture, not in the GOP terminal. */
    liteos_console_disable();
    liteos_realtest_checkpoint("POST_SCHEDULER_BEGIN");
    if (!liteos_init_post_scheduler(info, active_controller)) {
        halt_forever();
    }
    liteos_realtest_mark("POST_SCHEDULER_OK");
    liteos_realtest_checkpoint("POST_SCHEDULER_OK");

    liteos_realtest_mark("RUNTIME_ENTRY");
    liteos_realtest_checkpoint("RUNTIME_BEGIN");
    liteos_init_runtime_start(info, framebuffer_virtual_base);
}
