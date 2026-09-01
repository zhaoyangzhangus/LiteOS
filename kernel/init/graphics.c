#include <kernel/init_graphics.h>

#include <kernel/debug_stage.h>
#include <kernel/gpu.h>
#include <kernel/perf.h>
#include <kernel/telemetry.h>
#include <kernel/window_server.h>
#include <kernel/xhci.h>
#include <arch/x86_64/syscall_internal.h>

static BOOLEAN usb_self_test(void) {
    uint64_t benchmark_start = telemetry_timestamp();
    BOOLEAN event_queue_ok = xhci_event_queue_self_test();

    kernel_perf_emit_scope("io.xhci_events", benchmark_start);
    /* Hardware probing is owned by init_devices/xhci_hardware_self_test.
     * This boundary only keeps the protocol-independent xHCI primitives
     * deterministic and therefore does not need a second PCI/ring model. */
    if (!event_queue_ok ||
        !xhci_transfer_encode_self_test() ||
        !xhci_device_context_self_test() ||
        !xhci_endpoint_context_self_test()) return 0;
    return 1;
}

static BOOLEAN syscall_self_test(const LITEOS_BOOT_INFO *info) {
    syscall_frame_t frame;
    for (UINTN i = 0; i < sizeof(frame); ++i) ((UINT8 *)&frame)[i] = 0;
    frame.rip = 0x10000ULL;
    frame.rsp = 0x20000ULL;
    frame.cs = 0x23ULL;
    frame.ss = 0x1BULL;
    frame.rflags = 0x202ULL;
    frame.rax = OS_SYS_DEBUG_QUERY;
    return info != 0 && liteos_syscall_init(info->BootstrapStackTop) &&
           liteos_syscall_dispatch(&frame) == OS_SYSCALL_ABI_VERSION &&
           x86_syscall_return_mode(&frame) == 1;
}

static BOOLEAN graphics_fail_at(const liteos_init_graphics_hooks_t *hooks,
                                const CHAR8 *message, const char *file,
                                uint32_t line) {
    liteos_debug_stage_fail_at(LITEOS_DEBUG_PHASE_DISPLAY,
                               LITEOS_DEBUG_STEP_FAIL, K_EIO, file, line);
    hooks->write(message);
    /* GOP and GPU backends are optional.  Preserve the failure and continue
     * booting so a headless machine can still reach the shell and services. */
    return 0;
}

#define graphics_fail(hooks, message) \
    graphics_fail_at((hooks), (message), __FILE__, __LINE__)

BOOLEAN liteos_init_graphics(const LITEOS_BOOT_INFO *info,
                             const liteos_init_graphics_hooks_t *hooks) {
    uint64_t benchmark_start;
    if (info == 0 || hooks == 0 || hooks->write == 0 ||
        hooks->write_u32 == 0 || hooks->halt == 0) return 0;

    liteos_debug_stage_enter(LITEOS_DEBUG_PHASE_SPEC_13);
    liteos_debug_stage_enter(LITEOS_DEBUG_PHASE_SPEC_14);
    benchmark_start = telemetry_timestamp();
    if (!gpu_core_self_test()) {
        return graphics_fail(hooks, "LITEOS_GPU_CORE_FAIL\r\n");
    }
    kernel_perf_emit_scope("graphics.gpu", benchmark_start);
    hooks->write("LITEOS_GPU_OK\r\n");
    hooks->write("LITEOS_GPU_CORE_OK\r\n");
    benchmark_start = telemetry_timestamp();
    if (!usb_self_test()) {
        return graphics_fail(hooks, "LITEOS_USB_TEST_FAIL\r\n");
    }
    kernel_perf_emit_scope("io.xhci", benchmark_start);
    hooks->write("LITEOS_USB_OK\r\n");
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_8,
                       LITEOS_DEBUG_STEP_PROGRESS, 1U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_8,
                       LITEOS_DEBUG_STEP_PROGRESS, 5U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_8,
                       LITEOS_DEBUG_STEP_PROGRESS, 6U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_8,
                       LITEOS_DEBUG_STEP_PROGRESS, 8U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_8,
                       LITEOS_DEBUG_STEP_PROGRESS, 9U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_8,
                       LITEOS_DEBUG_STEP_PROGRESS, 10U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_8,
                       LITEOS_DEBUG_STEP_PROGRESS, 11U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_8,
                       LITEOS_DEBUG_STEP_PROGRESS, 12U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_8,
                       LITEOS_DEBUG_STEP_PROGRESS, 13U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_8,
                       LITEOS_DEBUG_STEP_PROGRESS, 14U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_8,
                       LITEOS_DEBUG_STEP_PROGRESS, 15U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_8,
                       LITEOS_DEBUG_STEP_PROGRESS, 16U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_8,
                       LITEOS_DEBUG_STEP_PROGRESS, 17U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_7B,
                       LITEOS_DEBUG_STEP_PROGRESS, 1U);
    /* Canonical compositor owns immutable snapshot capture and planning. */
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_7B,
                       LITEOS_DEBUG_STEP_PROGRESS, 2U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_7B,
                       LITEOS_DEBUG_STEP_PROGRESS, 3U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_7B,
                       LITEOS_DEBUG_STEP_PROGRESS, 4U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_7B,
                       LITEOS_DEBUG_STEP_PROGRESS, 5U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_7B,
                       LITEOS_DEBUG_STEP_PROGRESS, 6U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_7B,
                       LITEOS_DEBUG_STEP_PROGRESS, 7U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_7B,
                       LITEOS_DEBUG_STEP_PROGRESS, 8U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_7B,
                       LITEOS_DEBUG_STEP_PROGRESS, 9U);
    if (!compositor_tile_self_test()) {
        return graphics_fail(hooks, "LITEOS_TILE_METADATA_TEST_FAIL\r\n");
    }
    if (!desktop_alpha_self_test()) {
        return graphics_fail(hooks, "LITEOS_ALPHA_COMPOSITION_TEST_FAIL\r\n");
    }
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_7B,
                       LITEOS_DEBUG_STEP_PROGRESS, 10U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_7A,
                       LITEOS_DEBUG_STEP_PROGRESS, 1U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_7A,
                       LITEOS_DEBUG_STEP_PROGRESS, 2U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_7A,
                       LITEOS_DEBUG_STEP_PROGRESS, 3U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_7A,
                       LITEOS_DEBUG_STEP_PROGRESS, 4U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_7A,
                       LITEOS_DEBUG_STEP_PROGRESS, 5U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_7A,
                       LITEOS_DEBUG_STEP_PROGRESS, 6U);
    if (!window_input_router_self_test()) {
        return graphics_fail(hooks, "LITEOS_INPUT_ROUTER_TEST_FAIL\r\n");
    }
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_7A,
                       LITEOS_DEBUG_STEP_PROGRESS, 7U);
    if (!window_present_self_test()) {
        return graphics_fail(hooks, "LITEOS_PRESENT_TEST_FAIL\r\n");
    }
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_7A,
                       LITEOS_DEBUG_STEP_PROGRESS, 8U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_7A,
                       LITEOS_DEBUG_STEP_PROGRESS, 9U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_7A,
                       LITEOS_DEBUG_STEP_PROGRESS, 10U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_7A,
                       LITEOS_DEBUG_STEP_PROGRESS, 11U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_7A,
                       LITEOS_DEBUG_STEP_PROGRESS, 12U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_7A,
                       LITEOS_DEBUG_STEP_PROGRESS, 13U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_7A,
                       LITEOS_DEBUG_STEP_PROGRESS, 14U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_7A,
                       LITEOS_DEBUG_STEP_PROGRESS, 15U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_7A,
                       LITEOS_DEBUG_STEP_PROGRESS, 16U);
    liteos_debug_stage_ready(LITEOS_DEBUG_PHASE_SPEC_14);
    benchmark_start = telemetry_timestamp();
    if (!syscall_frame_self_test() || !syscall_self_test(info)) {
        return graphics_fail(hooks, "LITEOS_SYSCALL_TEST_FAIL\r\n");
    }
    kernel_perf_emit_scope("io.syscall", benchmark_start);
    hooks->write("LITEOS_SYSCALL_OK\r\n");
    liteos_debug_stage_ready(LITEOS_DEBUG_PHASE_SPEC_7);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_3,
                       LITEOS_DEBUG_STEP_PROGRESS, 9U);
    return 1;
}
