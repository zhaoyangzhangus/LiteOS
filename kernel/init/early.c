#include <kernel/init_early.h>

#include "arch/x86_64/apic.h"
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/interrupt.h>
#include <kernel/crash_dump.h>
#include <kernel/console.h>
#include <kernel/debug_stage.h>
#include <kernel/deferred.h>
#include <kernel/irq.h>
#include <kernel/mm_boot.h>
#include <kernel/power.h>
#include <kernel/random.h>
#include <kernel/service.h>
#include <kernel/watchdog.h>

static BOOLEAN early_fail_at(const liteos_init_early_hooks_t *hooks,
                             const CHAR8 *message, const char *file,
                             uint32_t line) {
    liteos_debug_stage_fail_at(LITEOS_DEBUG_PHASE_BOOT,
                               LITEOS_DEBUG_STEP_FAIL, K_EIO, file, line);
    hooks->write(message);
    hooks->halt();
    return 0;
}

#define early_fail(hooks, message) \
    early_fail_at((hooks), (message), __FILE__, __LINE__)

static BOOLEAN apic_self_test(void) {
    return liteos_lapic_init(32U, 1000000U) &&
           liteos_lapic_tick_count() == 0;
}

static void publish_refactor_boot_snapshot(void) {
    /* Phase 0 is intentionally not claimed until a repeatable benchmark
     * baseline is checked in.  The remaining records make the current
     * frontier visible even when a later phase has not started yet. */
    liteos_debug_stage_pending(LITEOS_DEBUG_PHASE_REFACTOR_0);
    liteos_debug_stage_ready(LITEOS_DEBUG_PHASE_REFACTOR_1);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_2,
                       LITEOS_DEBUG_STEP_ENTER, 0U);
    liteos_debug_stage_pending(LITEOS_DEBUG_PHASE_REFACTOR_3);
    liteos_debug_stage_pending(LITEOS_DEBUG_PHASE_REFACTOR_4);
    liteos_debug_stage_pending(LITEOS_DEBUG_PHASE_REFACTOR_5);
    liteos_debug_stage_pending(LITEOS_DEBUG_PHASE_REFACTOR_6);
    liteos_debug_stage_pending(LITEOS_DEBUG_PHASE_REFACTOR_7A);
    liteos_debug_stage_pending(LITEOS_DEBUG_PHASE_REFACTOR_7B);
    liteos_debug_stage_pending(LITEOS_DEBUG_PHASE_REFACTOR_8);
    liteos_debug_stage_pending(LITEOS_DEBUG_PHASE_REFACTOR_9);
}

BOOLEAN liteos_init_early(LITEOS_BOOT_INFO *info,
                          const liteos_init_early_hooks_t *hooks) {
    if (info == 0 || hooks == 0 || hooks->write == 0 || hooks->halt == 0) {
        return 0;
    }
    if (info->Magic != LITEOS_BOOTINFO_MAGIC ||
        info->Version < LITEOS_BOOTINFO_VERSION ||
        info->Size < sizeof(*info)) {
        return early_fail(hooks, "LITEOS_KERNEL_BAD_BOOTINFO\r\n");
    }

    UINT64 kernel_end = info->KernelPhysicalBase + info->KernelSize;
    UINT64 kernel_virtual_end = info->KernelVirtualBase + info->KernelSize;
    if (info->KernelPhysicalBase == 0 || info->KernelSize == 0 ||
        kernel_end < info->KernelPhysicalBase || info->KernelVirtualBase == 0 ||
        kernel_virtual_end < info->KernelVirtualBase ||
        info->KernelEntry < info->KernelVirtualBase ||
        info->KernelEntry >= kernel_virtual_end) {
        return early_fail(hooks, "LITEOS_KERNEL_BAD_RANGE\r\n");
    }
    if ((info->Flags & LITEOS_BOOTINFO_HAS_BOOT_DEVICE) == 0 ||
        info->BootDevicePathSize < sizeof(EFI_DEVICE_PATH_PROTOCOL)) {
        return early_fail(hooks, "LITEOS_BOOT_DEVICE_FAIL\r\n");
    }
    hooks->write("LITEOS_KERNEL_OK\r\n");
    hooks->write("LITEOS_BOOT_DEVICE_OK\r\n");
    hooks->write("Hello World!\r\n");

    if (!liteos_arch_cpu_init()) {
        return early_fail(hooks, "LITEOS_CPU_INIT_FAIL\r\n");
    }
    liteos_random_init(info);
    liteos_serial_enable_concurrency();
    hooks->write("LITEOS_CPU_INIT_OK\r\n");
    liteos_debug_stage(LITEOS_DEBUG_PHASE_BOOT,
                       LITEOS_DEBUG_STEP_READY, 1U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_CPU,
                       LITEOS_DEBUG_STEP_READY, 1U);
    publish_refactor_boot_snapshot();
    liteos_debug_stage_enter(LITEOS_DEBUG_PHASE_SPEC_0);
    liteos_debug_stage_ready(LITEOS_DEBUG_PHASE_SPEC_0);
    liteos_debug_stage_enter(LITEOS_DEBUG_PHASE_SPEC_1);
    liteos_debug_stage_enter(LITEOS_DEBUG_PHASE_SPEC_16);
    liteos_debug_stage_enter(LITEOS_DEBUG_PHASE_SPEC_17);
    liteos_debug_stage_enter(LITEOS_DEBUG_PHASE_SPEC_18);

    if (!x86_cpu_hardening_self_test()) {
        return early_fail(hooks, "LITEOS_HARDENING_FAIL\r\n");
    }
    hooks->write("LITEOS_HARDENING_OK\r\n");
    if (!liteos_arch_set_kernel_stack(info->BootstrapStackBase,
                                      info->BootstrapStackSize)) {
        return early_fail(hooks, "LITEOS_TSS_STACK_FAIL\r\n");
    }
    hooks->write("LITEOS_TSS_OK\r\n");
    if (!x86_exception_self_test()) {
        return early_fail(hooks, "LITEOS_EXCEPTION_TEST_FAIL\r\n");
    }
    hooks->write("LITEOS_EXCEPTION_OK\r\n");
    liteos_debug_stage_ready(LITEOS_DEBUG_PHASE_SPEC_1);
    if (!apic_self_test()) {
        return early_fail(hooks, "LITEOS_APIC_TEST_FAIL\r\n");
    }
    hooks->write("LITEOS_APIC_OK\r\n");
    if (!deferred_init() || !deferred_self_test()) {
        return early_fail(hooks, "LITEOS_DEFERRED_WORK_FAIL\r\n");
    }
    hooks->write("LITEOS_DEFERRED_WORK_OK\r\n");
    if (!service_manager_init() || !service_manager_self_test()) {
        return early_fail(hooks, "LITEOS_SERVICE_CORE_FAIL\r\n");
    }
    hooks->write("LITEOS_SERVICE_CORE_OK\r\n");
    if (!watchdog_manager_init() || !watchdog_self_test()) {
        return early_fail(hooks, "LITEOS_WATCHDOG_FAIL\r\n");
    }
    hooks->write("LITEOS_WATCHDOG_OK\r\n");
    if (!power_manager_init() || !power_self_test()) {
        return early_fail(hooks, "LITEOS_POWER_CORE_FAIL\r\n");
    }
    hooks->write("LITEOS_POWER_CORE_OK\r\n");
    if (!crash_dump_self_test()) {
        return early_fail(hooks, "LITEOS_CRASH_DUMP_FAIL\r\n");
    }
    hooks->write("LITEOS_CRASH_DUMP_OK\r\n");
    liteos_debug_stage_ready(LITEOS_DEBUG_PHASE_SPEC_17);
    if (!irq_core_self_test()) {
        return early_fail(hooks, "LITEOS_IRQ_CORE_FAIL\r\n");
    }
    hooks->write("LITEOS_IRQ_CORE_OK\r\n");
    if (!liteos_enable_kernel_paging(info)) {
        return early_fail(hooks, "LITEOS_PAGING_INIT_FAIL\r\n");
    }
    /* The identity alias remains active until the RAM allocator installs the
     * final direct map and framebuffer alias in the next init phase. */
    hooks->write("LITEOS_PAGING_OK\r\n");
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_3,
                       LITEOS_DEBUG_STEP_PROGRESS, 1U);
    return 1;
}
