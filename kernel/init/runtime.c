#include <kernel/bootinfo.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/interrupt.h>
#include <arch/x86_64/paging.h>
#include <arch/x86_64/uaccess.h>
#include <arch/x86_64/apic.h>
#include <arch/x86_64/acpi.h>
#include <arch/x86_64/smp.h>
#include <arch/x86_64/context.h>
#include <kernel/block_device.h>
#include <kernel/mm_boot.h>
#include <kernel/kmem.h>
#include <kernel/handle.h>
#include <kernel/mutex.h>
#include <kernel/object.h>
#include <kernel/audio.h>
#include <kernel/hda.h>
#include <kernel/bluetooth.h>
#include <kernel/package.h>
#include <kernel/firmware.h>
#include <kernel/rcu.h>
#include <kernel/telemetry.h>
#include <kernel/perf.h>
#include <kernel/realtest.h>
#include <kernel/dma.h>
#include <kernel/gpu.h>
#include <kernel/display.h>
#include <kernel/qemu_stdvga.h>
#include <kernel/iommu.h>
#include <kernel/device.h>
#include <kernel/io.h>
#include <kernel/completion_port.h>
#include <kernel/input.h>
#include <kernel/window_server.h>
#include <kernel/message_port.h>
#include <kernel/timer.h>
#include <kernel/deferred.h>
#include <kernel/service.h>
#include <kernel/power.h>
#include <kernel/block.h>
#include <kernel/pci.h>
#include <kernel/process.h>
#include <kernel/resource.h>
#include <kernel/elf_loader.h>
#include <kernel/init_runtime.h>
#include <kernel/init_memory.h>
#include <kernel/init_userspace.h>

#ifndef OS_AUTOSTART_CHROME
#define OS_AUTOSTART_CHROME 0
#endif
#ifndef OS_INTERACTIVE_DESKTOP
#define OS_INTERACTIVE_DESKTOP 0
#endif
#include <kernel/init_user_services.h>
#include <kernel/wait.h>
#include <kernel/vm.h>
#include <kernel/vfs.h>
#include <kernel/journal.h>
#include <kernel/litefs.h>

#include "../drivers/gpu/nvidia/rm/include/nv_rm_driver.h"

#if defined(OS_NVKMS_W3) && OS_NVKMS_W3
#include <kernel/nvidia/nvkms.h>
#endif

#ifndef OS_NVIDIA_REALTEST_NO_DESKTOP
#define OS_NVIDIA_REALTEST_NO_DESKTOP 0
#endif
#ifndef OS_NVIDIA_PHASE5
#define OS_NVIDIA_PHASE5 0
#endif
#ifndef OS_PHASE7
#define OS_PHASE7 0
#endif
#ifndef OS_PHASE8
#define OS_PHASE8 0
#endif
#ifndef OS_PHASE9
#define OS_PHASE9 0
#endif
#ifndef OS_PHASE10
#define OS_PHASE10 0
#endif
#ifndef OS_PHASE11
#define OS_PHASE11 0
#endif
#ifndef OS_PHASE12
#define OS_PHASE12 0
#endif
#ifndef OS_PHASE13
#define OS_PHASE13 0
#endif
#ifndef OS_NVKMS_W3
#define OS_NVKMS_W3 0
#endif
#include <kernel/e1000.h>
#include <kernel/net_core.h>
#include <kernel/net_manager.h>
#include <kernel/socket.h>
#include <kernel/console.h>
#include <kernel/irq.h>
#include <kernel/nvme_core.h>
#include <kernel/rtl8126.h>
#include <ascii_font.h>
#include <arch/x86_64/syscall_internal.h>
#include <usb/storage.h>
#include <kernel/sched.h>
#include <kernel/debug_stage.h>
#include <kernel/console_backend.h>

#ifndef OS_DEBUG_SERIAL
#define OS_DEBUG_SERIAL 0
#endif

/* REFACTOR_P3_RUNTIME_OWNER: post-init high-half continuation. */
static UINT64 g_framebuffer_virtual_base;

#define halt_forever() kernel_halt_forever_at(__FILE__, __LINE__)
#define OS_REALTEST_NETWORK_TIMEOUT_NS 15000000000ULL

static void __attribute__((noreturn)) kernel_runtime_main(void *context) {
    OS_BOOT_INFO *info = (OS_BOOT_INFO *)context;
    UINT64 benchmark_start;
    UINT64 direct_span = X86_64_DIRECT_MAP_END - X86_64_DIRECT_MAP_BASE + 1ULL;
#if OS_REALTEST
    /* This is the first C-side instruction after the high-stack call.  Keep
     * the checkpoint before dereferencing the handoff so a missing marker
     * distinguishes entry/prologue failure from a bad handoff pointer. */
    realtest_checkpoint("RUNTIME_HIGH_ENTRY");
#endif
    if (info == 0 || info->BootstrapStackBase >= direct_span ||
        info->BootstrapStackSize > direct_span - info->BootstrapStackBase) {
        kernel_serial_write("OS_HIGH_STACK_FAIL\r\n");
        halt_forever();
    }

    UINT64 high_stack_base = X86_64_DIRECT_MAP_BASE + info->BootstrapStackBase;
    if (!arch_set_kernel_stack(high_stack_base, info->BootstrapStackSize) ||
        !sched_set_boot_kernel_stack(x86_tss_get_rsp0())) {
        kernel_serial_write("OS_HIGH_STACK_FAIL\r\n");
        halt_forever();
    }
#if OS_REALTEST
    realtest_checkpoint("RUNTIME_HIGH_STACK_CONFIGURED");
#endif
    x86_syscall_set_kernel_stack(high_stack_base + info->BootstrapStackSize);
#if OS_REALTEST
    realtest_checkpoint("RUNTIME_HIGH_SYSCALL_STACK_OK");
    realtest_checkpoint("RUNTIME_HIGH_FRAMEBUFFER_CHECK_BEGIN");
    kernel_serial_printf_serial_only(
        "OS_RUNTIME_FRAMEBUFFER_FIELDS flags_lo=%u flags_hi=%u width=%u "
        "height=%u stride=%u size_lo=%u size_hi=%u base_lo=%u base_hi=%u\r\n",
        (UINT32)info->Flags, (UINT32)(info->Flags >> 32U),
        info->FrameBufferWidth, info->FrameBufferHeight,
        info->FrameBufferPixelsPerScanLine,
        (UINT32)info->FrameBufferSize,
        (UINT32)(info->FrameBufferSize >> 32U),
        (UINT32)info->FrameBufferBase,
        (UINT32)(info->FrameBufferBase >> 32U));
    realtest_checkpoint("RUNTIME_HIGH_FRAMEBUFFER_FIELDS");
#endif

    if ((info->Flags & OS_BOOTINFO_HAS_FRAMEBUFFER) == 0 ||
        info->FrameBufferWidth == 0 || info->FrameBufferHeight == 0 ||
        info->FrameBufferPixelsPerScanLine < info->FrameBufferWidth) {
#if OS_REALTEST
        realtest_capture("OS_FRAMEBUFFER_MAP_FAIL\r\n");
        realtest_finish_failure();
#endif
        kernel_serial_write("OS_FRAMEBUFFER_MAP_FAIL\r\n");
        halt_forever();
    }
    UINT64 required_pixels = (UINT64)info->FrameBufferPixelsPerScanLine *
                             info->FrameBufferHeight;
    if (required_pixels > UINT64_MAX / sizeof(UINT32) ||
        required_pixels * sizeof(UINT32) > info->FrameBufferSize) {
#if OS_REALTEST
        realtest_capture("OS_FRAMEBUFFER_MAP_FAIL\r\n");
        realtest_finish_failure();
#endif
        kernel_serial_write("OS_FRAMEBUFFER_MAP_FAIL\r\n");
        halt_forever();
    }
#if OS_REALTEST
    realtest_checkpoint("RUNTIME_HIGH_FRAMEBUFFER_VALIDATED");
#endif

    /* Keep the firmware-owned scanout description separate from the kernel
     * virtual alias and the userspace XRGB8888 surface contract.  This is a
     * one-time diagnostic because a valid CPU-side render/readback does not
     * by itself prove that the physical GOP target is the active scanout. */
    kernel_serial_printf_serial_only(
        "OS_FRAMEBUFFER_INFO base_lo=%u base_hi=%u size_lo=%u size_hi=%u "
        "width=%u height=%u stride=%u format=%u mask0=%u mask1=%u mask2=%u mask3=%u\r\n",
        (uint32_t)info->FrameBufferBase,
        (uint32_t)(info->FrameBufferBase >> 32U),
        (uint32_t)info->FrameBufferSize,
        (uint32_t)(info->FrameBufferSize >> 32U),
        info->FrameBufferWidth,
        info->FrameBufferHeight,
        info->FrameBufferPixelsPerScanLine,
        info->FrameBufferFormat,
        info->FrameBufferMask[0],
        info->FrameBufferMask[1],
        info->FrameBufferMask[2],
        info->FrameBufferMask[3]);
#if OS_REALTEST
    realtest_checkpoint("RUNTIME_HIGH_FRAMEBUFFER_INFO_OK");
#endif

    UINT64 framebuffer_virtual = g_framebuffer_virtual_base;
#if OS_REALTEST
    realtest_checkpoint("RUNTIME_HIGH_FRAMEBUFFER_ALIAS_BEGIN");
#endif
    if (framebuffer_virtual == 0U &&
        !map_framebuffer_wc(info, &framebuffer_virtual)) {
#if OS_REALTEST
        realtest_capture("OS_FRAMEBUFFER_MAP_FAIL\r\n");
        realtest_finish_failure();
#endif
        kernel_serial_write("OS_FRAMEBUFFER_MAP_FAIL\r\n");
        halt_forever();
    }
#if OS_REALTEST
    realtest_checkpoint("RUNTIME_HIGH_FRAMEBUFFER_ALIAS_READY");
    realtest_checkpoint("RUNTIME_HIGH_CONSOLE_DISABLE_BEGIN");
#endif
    /* The GOP terminal is an early-boot diagnostic surface only.  Do not
     * recreate it here: the framebuffer is now owned by display/window code,
     * and the physical realtest path records diagnostics on the FAT volume. */
    kernel_console_disable();
#if OS_REALTEST
    realtest_checkpoint("RUNTIME_HIGH_CONSOLE_DISABLED");
#endif
    kernel_serial_write_serial_only("OS_GOP_CONSOLE_DISABLED\r\n");
    OS_BOOT_INFO display_info;
    for (UINTN byte = 0; byte < sizeof(display_info); ++byte) {
        ((UINT8 *)&display_info)[byte] = ((const UINT8 *)info)[byte];
    }
    display_info.FrameBufferBase = framebuffer_virtual;
#if OS_REALTEST
    realtest_checkpoint("RUNTIME_HIGH_BOOTINFO_COPY_OK");
#endif

    if (!display_core_init(display_info.FrameBufferBase, display_info.FrameBufferSize,
                           display_info.FrameBufferWidth, display_info.FrameBufferHeight,
                           display_info.FrameBufferPixelsPerScanLine,
                           display_info.FrameBufferFormat, display_info.FrameBufferMask)) {
#if OS_REALTEST
        realtest_capture("OS_DISPLAY_REMAP_FAIL\r\n");
        realtest_finish_failure();
#endif
        kernel_serial_write("OS_DISPLAY_REMAP_FAIL\r\n");
        halt_forever();
    }
#if OS_REALTEST
    realtest_checkpoint("RUNTIME_HIGH_DISPLAY_INIT_OK");
#endif
    if (!display_core_self_test()) {
#if OS_REALTEST
        realtest_capture("OS_DISPLAY_CORE_FAIL\r\n");
        realtest_finish_failure();
#endif
        kernel_serial_write("OS_DISPLAY_CORE_FAIL\r\n");
        halt_forever();
    }
#if OS_REALTEST
    realtest_checkpoint("RUNTIME_HIGH_DISPLAY_SELFTEST_OK");
#endif
    kernel_serial_write("OS_DISPLAY_CORE_OK\r\n");
    kernel_debug_stage(OS_DEBUG_PHASE_DISPLAY,
                       OS_DEBUG_STEP_READY, 1U);
    kernel_debug_stage_ready(OS_DEBUG_PHASE_SPEC_13);
#if OS_REALTEST
    realtest_checkpoint("RUNTIME_HIGH_DISPLAY_POST_OK");
#endif

    /*
     * Optional QEMU Standard VGA page-flip backend. Physical machines simply
     * miss PCI 1234:1111 and keep the existing GOP path.
     */
    if (qemu_stdvga_flip_init(
            info->FrameBufferBase,
            framebuffer_virtual,
            info->FrameBufferSize)) {

        kernel_serial_write("OS_QEMU_STDVGA_FLIP_OK\r\n");

    } else if (qemu_stdvga_hardware_present()) {
        kernel_serial_write("OS_QEMU_STDVGA_FLIP_DISABLED ERR=");
        kernel_serial_write_u32(qemu_stdvga_last_error());
        kernel_serial_write("\r\n");
    }
#if OS_REALTEST
    realtest_checkpoint("RUNTIME_QEMU_PROBE_DONE");
#endif

    realtest_disable_runtime_services();
#if OS_REALTEST
    realtest_checkpoint("RUNTIME_SERVICES_DISABLED");
#endif
    if (!arch_drop_identity_mapping()) {
        kernel_serial_write("OS_IDENTITY_REMOVE_FAIL\r\n");
        halt_forever();
    }
    paddr_t unexpected_mapping;
    if (x86_translate_page(x86_current_root_table(), 0, &unexpected_mapping, 0) != K_ENOENT) {
        kernel_serial_write("OS_IDENTITY_REMOVE_FAIL\r\n");
        halt_forever();
    }
    kernel_serial_write("OS_IDENTITY_REMOVED_OK\r\n");
#if OS_REALTEST
    realtest_checkpoint("RUNTIME_IDENTITY_REMOVED");
#endif

    /* Keep the framebuffer probe serial-only. */
    kernel_serial_write("OS_FRAMEBUFFER_UC_OK\r\n");
#if OS_REALTEST
    realtest_checkpoint("RUNTIME_FRAMEBUFFER_PROBE_OK");
#endif
#if !OS_AUTOSTART_CHROME && !OS_INTERACTIVE_DESKTOP
    kernel_init_userspace_hooks_t runtime_userspace_hooks = {
        .write = kernel_serial_write,
        .write_u32 = kernel_serial_write_u32,
        .halt = kernel_halt_forever,
    };
#endif
#if OS_AUTOSTART_CHROME
    kernel_serial_write("OS_USER_RUNTIME_SKIPPED\r\n");
#elif OS_INTERACTIVE_DESKTOP
    kernel_serial_write("OS_USER_RUNTIME_SKIPPED_FOR_INTERACTIVE_DESKTOP\r\n");
#else
    userspace_run_runtime_self_test(&runtime_userspace_hooks);
#endif
    kernel_serial_write("OS_DISPLAY_COMMIT_OK\r\n");
#if OS_REALTEST
    realtest_checkpoint("RUNTIME_USERSPACE_SELFTEST_OK");
    realtest_checkpoint("RUNTIME_SMP_USER_DISPATCH_BEGIN");
#endif
    bool smp_user_dispatch_ok = true;
#if !OS_AUTOSTART_CHROME && !OS_INTERACTIVE_DESKTOP
    smp_user_dispatch_ok = x86_smp_remote_user_self_test();
#elif OS_AUTOSTART_CHROME
    kernel_serial_write("OS_SMP_USER_DISPATCH_SKIPPED_FOR_CHROME\r\n");
#else
    kernel_serial_write("OS_SMP_USER_DISPATCH_SKIPPED_FOR_INTERACTIVE_DESKTOP\r\n");
#endif
#if OS_REALTEST
    realtest_checkpoint(smp_user_dispatch_ok ?
                               "RUNTIME_SMP_USER_DISPATCH_OK" :
                               "RUNTIME_SMP_USER_DISPATCH_FAIL");
#endif
    if (!smp_user_dispatch_ok) {
        kernel_serial_write("OS_SMP_USER_DISPATCH_FAIL\r\n");
        halt_forever();
    }
    kernel_serial_write("OS_SMP_USER_DISPATCH_OK\r\n");
    nv_rm_driver_set_screen_info(
        info->FrameBufferBase, info->FrameBufferWidth,
        info->FrameBufferHeight, 32U,
        info->FrameBufferPixelsPerScanLine * 4U,
        info->FrameBufferSize);
#if OS_PHASE13
    kernel_serial_write_serial_only("OS_P13_LINUX_EVENTS_BEGIN\r\n");
    if (!linux_phase13_realtest()) {
        kernel_serial_write_serial_only("OS_P13_LINUX_EVENTS_FAIL\r\n");
#if OS_REALTEST
        realtest_mark("OS_P13_LINUX_EVENTS_FAIL");
        realtest_finish_failure();
#else
        halt_forever();
#endif
    }
    kernel_serial_write_serial_only("OS_P13_LINUX_EVENTS_PASS\r\n");
#if OS_REALTEST
        realtest_finish_success();
#endif
#elif OS_PHASE12
    if (nv_rm_driver_probe() != K_OK ||
        nv_rm_driver_current() == 0 ||
        nv_rm_driver_current()->state != NV_GPU_GSP_READY) {
        kernel_serial_write_serial_only("OS_P12_NVIDIA_GSP_FAIL\r\n");
        realtest_mark("OS_P12_NVIDIA_GSP_FAIL");
        realtest_finish_failure();
    }
    kernel_serial_write_serial_only("OS_P12_NVIDIA_GSP_READY\r\n");
    if (realtest_enabled()) {
        kernel_serial_write_serial_only("OS_P12_WINDOW_BEGIN\r\n");
        if (!linux_phase12_realtest()) {
            kernel_serial_write_serial_only("OS_P12_FAIL\r\n");
            realtest_mark("OS_P12_FAIL");
            realtest_finish_failure();
        }
        realtest_finish_success();
    }
#elif OS_PHASE11
    if (nv_rm_driver_probe() != K_OK ||
        nv_rm_driver_current() == 0 ||
        nv_rm_driver_current()->state != NV_GPU_GSP_READY) {
        kernel_serial_write_serial_only("OS_P11_NVIDIA_GSP_FAIL\r\n");
        realtest_mark("OS_P11_NVIDIA_GSP_FAIL");
        realtest_finish_failure();
    }
    kernel_serial_write_serial_only("OS_P11_NVIDIA_GSP_READY\r\n");
    if (realtest_enabled()) {
        kernel_serial_write_serial_only("OS_P11_TRIANGLE_BEGIN\r\n");
        if (!linux_phase11_realtest()) {
            kernel_serial_write_serial_only("OS_P11_FAIL\r\n");
            realtest_mark("OS_P11_FAIL");
            realtest_finish_failure();
        }
        realtest_finish_success();
    }
#elif OS_PHASE10
    if (nv_rm_driver_probe() != K_OK ||
        nv_rm_driver_current() == 0 ||
        nv_rm_driver_current()->state != NV_GPU_GSP_READY) {
        kernel_serial_write_serial_only("OS_P10_NVIDIA_GSP_FAIL\r\n");
        realtest_mark("OS_P10_NVIDIA_GSP_FAIL");
        realtest_finish_failure();
    }
    kernel_serial_write_serial_only("OS_P10_NVIDIA_GSP_READY\r\n");
    if (realtest_enabled()) {
        kernel_serial_write_serial_only("OS_P10_EGL_BEGIN\r\n");
        if (!linux_phase10_realtest()) {
            kernel_serial_write_serial_only("OS_P10_EGL_FAIL\r\n");
            realtest_mark("OS_P10_EGL_FAIL");
            realtest_finish_failure();
        }
        kernel_serial_write_serial_only("OS_P10_EGL_PROCESS_PASS\r\n");
        realtest_finish_success();
    }
#elif OS_PHASE9
    if (nv_rm_driver_probe() != K_OK ||
        nv_rm_driver_current() == 0 ||
        nv_rm_driver_current()->state != NV_GPU_GSP_READY) {
        kernel_serial_write_serial_only("OS_P9_NVIDIA_GSP_FAIL\r\n");
        realtest_mark("OS_P9_NVIDIA_GSP_FAIL");
        realtest_finish_failure();
    }
    kernel_serial_write_serial_only("OS_P9_NVIDIA_GSP_READY\r\n");
    if (realtest_enabled()) {
        kernel_serial_write_serial_only("OS_P9_USERSPACE_BEGIN\r\n");
        if (!linux_phase9_realtest()) {
            kernel_serial_write_serial_only("OS_P9_USERSPACE_FAIL\r\n");
            realtest_mark("OS_P9_USERSPACE_FAIL");
            realtest_finish_failure();
        }
        kernel_serial_write_serial_only("OS_P9_USERSPACE_PROCESS_PASS\r\n");
        realtest_finish_success();
    }
#elif OS_PHASE8
    if (nv_rm_driver_probe() != K_OK ||
        nv_rm_driver_current() == 0 ||
        nv_rm_driver_current()->state != NV_GPU_GSP_READY) {
        kernel_serial_write_serial_only("OS_P8_NVIDIA_GSP_FAIL\r\n");
        realtest_mark("OS_P8_NVIDIA_GSP_FAIL");
        realtest_finish_failure();
    }
    kernel_serial_write_serial_only("OS_P8_NVIDIA_GSP_READY\r\n");
    if (realtest_enabled()) {
        kernel_serial_write_serial_only("OS_P8_FRONTEND_BEGIN\r\n");
        if (!linux_phase8_realtest()) {
            kernel_serial_write_serial_only("OS_P8_FRONTEND_FAIL\r\n");
            realtest_mark("OS_P8_FRONTEND_FAIL");
            realtest_finish_failure();
        }
        kernel_serial_write_serial_only("OS_P8_FRONTEND_PROCESS_PASS\r\n");
        realtest_finish_success();
    }
#elif OS_PHASE7
    if (realtest_enabled()) {
        kernel_serial_write_serial_only("OS_P7_GLIBC_BEGIN\r\n");
        if (!linux_phase7_realtest()) {
            kernel_serial_write_serial_only("OS_P7_GLIBC_FAIL\r\n");
            realtest_mark("OS_P7_GLIBC_FAIL");
            realtest_finish_failure();
        }
        kernel_serial_write_serial_only("OS_P7_GLIBC_PROCESS_PASS\r\n");
        realtest_finish_success();
    }
#endif
#if OS_NVIDIA_PHASE5
    {
        kstatus_t nvidia_probe_status;
        nvidia_probe_status = nv_rm_driver_probe();
        if (nvidia_probe_status != K_OK ||
            nv_rm_driver_current() == 0 ||
            nv_rm_driver_current()->state != NV_GPU_GSP_READY) {
            kernel_serial_write_serial_only("OS_NVIDIA_GSP_FAIL\r\n");
            realtest_mark("OS_NVIDIA_GSP_FAIL");
            if (realtest_enabled())
                realtest_finish_failure();
            /* A production desktop must retain GOP and services when the
             * optional GPU is absent or fails.  Do not claim GSP readiness. */
        } else {
            kernel_serial_write_serial_only("OS_NVIDIA_GSP_READY\r\n");
        }
#if !OS_NVKMS_W3
        if (realtest_enabled()) {
            kernel_serial_write_serial_only("OS_NVIDIA_PHASE5_GATE_READY\r\n");
            realtest_finish_success();
        }
#endif
    }
#elif OS_NVIDIA_REALTEST_NO_DESKTOP
    if (realtest_enabled()) {
        const nv_gpu_device_t *nvidia_gpu = nv_rm_driver_current();
        if (nvidia_gpu == 0 || nvidia_gpu->state != NV_GPU_HAL_READY) {
            kernel_serial_write_serial_only("OS_NVIDIA_PHASE4_FAIL\r\n");
            realtest_mark("OS_NVIDIA_PHASE4_FAIL");
            realtest_finish_failure();
        }
        kernel_serial_write_serial_only("OS_NVIDIA_PHASE4_GATE_READY\r\n");
        realtest_finish_success();
    }
#endif
#if OS_NVKMS_W3
    {
        kstatus_t nvkms_status = nv_nvkms_bootstrap_init();
        if (nvkms_status != K_OK) {
            /* A realtest failure must be terminal.  Continuing through the
             * GOP path here would let the later generic completion path claim
             * success even though the requested native modeset did not run. */
            kernel_serial_printf_serial_only(
                "OS_NVKMS_NATIVE_SCANOUT_FALLBACK status=%d\r\n",
                (int)nvkms_status);
            if (realtest_enabled()) {
                realtest_mark("OS_NVKMS_NATIVE_SCANOUT_FAIL");
                realtest_finish_failure();
            }
        }
    }
#endif
    if (!window_server_kernel_ready()) {
        kernel_serial_write("OS_WINDOW_SERVER_KERNEL_FAIL\r\n");
        halt_forever();
    }
    vm_fault_telemetry_reset();
    kernel_serial_write("OS_WINDOW_SERVER_KERNEL_OK\r\n");
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_7A,
                       OS_DEBUG_STEP_ENTER, 0U);
    benchmark_start = telemetry_timestamp();
    if (!window_lifecycle_self_test()) {
        kernel_serial_write("OS_WINDOW_TEST_FAIL\r\n");
        halt_forever();
    }
    kernel_perf_emit_scope("graphics.window", benchmark_start);
    kernel_serial_write("OS_WINDOW_OK\r\n");
    /* Exclude boot graphics self-tests from the first real window sample. */
    vm_fault_telemetry_reset();
    kernel_init_userspace_hooks_t deferred_userspace_hooks = {
        .write = kernel_serial_write,
        .write_u32 = kernel_serial_write_u32,
        .halt = kernel_halt_forever,
    };
    if (x86_smp_discovered_count() <= 1U &&
        !userspace_start_window_worker(&deferred_userspace_hooks)) {
        halt_forever();
    }
    /*
     * Keep the optional artwork read out of the boot-critical hand-off.  On
     * a one-CPU machine the worker shares the BSP with the UAS/VFS path and
     * can take an unbounded amount of time from the runtime test's point of
     * view.  The compositor already has a solid hold surface, so publish the
     * desktop-running result first and start artwork loading afterwards.
     */
    kernel_console_disable();
    bool realtest_network_required =
        realtest_enabled() && rtl8126_hardware_present();
    bool realtest_success_reported = false;
    uint64_t realtest_network_deadline = 0U;
    if (realtest_network_required) {
        uint64_t timeout = x86_timeout_ns_to_tsc(
            OS_REALTEST_NETWORK_TIMEOUT_NS);
        realtest_network_deadline = timeout == UINT64_MAX ? UINT64_MAX :
                                    x86_read_tsc() + timeout;
        realtest_checkpoint("OS_REALTEST_WAIT_DHCP");
    } else if (realtest_enabled()) {
        realtest_finish_success();
        realtest_success_reported = true;
    }
    (void)window_server_start_asset_worker();
    /*
     * 启动自测完成后，BSP 仍需保留一个真正的 Ring0 普通上下文。
     * LAPIC 中断只投递 deferred work；这里在可抢占的内核上下文中消费
     * 它，尤其是 xHCI 的同步控制传输不能直接放进硬中断处理函数。
     * HLT 由下一次时钟中断唤醒，因此设备状态轮询不会依赖用户态 syscall。
     */
    uint32_t reported_ipv4 = 0U;
    bool reported_link_valid = false;
    bool reported_link_up = false;
    for (;;) {
        net_manager_status_t net_status;
        /* The BSP keeps its bootstrap/idle C continuation. Service local
         * command refs here too, even when no runnable thread needs a switch;
         * destructors must never run in the following CLI sleep decision. */
        sched_finish_switch();
        /* A device critical section may defer an IPI into the level-triggered
         * pending bit. Subsequent wakes then coalesce without another IPI.
         * Never sleep with that request already pending. CLI closes the
         * check-to-sleep race; a new hardware IPI wakes the atomic STI;HLT. */
        __asm__ volatile ("cli" : : : "memory");
        if (x86_smp_take_reschedule_request() || sched_runnable_count() != 0U) {
            __asm__ volatile ("sti" : : : "memory");
            (void)sched_try_run_ready();
        } else {
            __asm__ volatile ("sti; hlt" : : : "memory");
        }
        /*
         * The BSP remains on this bootstrap continuation after the normal
         * scheduler starts, while its queue.current is still the logical
         * idle thread.  AP idle loops consume reschedule IPIs themselves;
         * poll the same request here so a READY deferred worker cannot wait
         * for an unrelated timer tick on the BSP.
         */
        /* Remote wakes are still in the command inbox and are not counted
         * as runnable until sched_try_run_ready drains it. Do not consume
         * and discard that request merely because the published rq is empty. */
        if (x86_smp_take_reschedule_request() || sched_runnable_count() != 0U) {
            (void)sched_try_run_ready();
        }
        /* Persist post-desktop HID/window diagnostics without doing any I/O
         * when the real-test buffer has not changed. */
        if (realtest_enabled()) (void)realtest_flush();
        net_manager_poll();
        if (net_manager_get_status(&net_status)) {
            /* 链路状态由 net_manager 统一采样。 */
            if (net_status.hardware_present) {
                if (reported_link_valid && reported_link_up != net_status.link_up) {
                    kernel_serial_write(net_status.link_up ?
                                        "OS_NET_LINK_UP\r\n" :
                                        "OS_NET_LINK_DOWN\r\n");
                }
                reported_link_valid = true;
                reported_link_up = net_status.link_up;
            } else {
                reported_link_valid = false;
            }
            if (net_status.ipv4_address != 0U &&
                net_status.ipv4_prefix_length != 0U &&
                reported_ipv4 == 0U) {
                kernel_serial_write("OS_NET_DHCP_OK ");
                kernel_serial_write_u32(net_status.ipv4_address);
                kernel_serial_write("/");
                kernel_serial_write_u32(net_status.ipv4_prefix_length);
                kernel_serial_write("\r\n");
                reported_ipv4 = net_status.ipv4_address;
                if (realtest_network_required && !realtest_success_reported) {
                    realtest_checkpoint("OS_REALTEST_NETWORK_OK");
                    realtest_finish_success();
                    realtest_success_reported = true;
                }
            } else if (net_status.ipv4_address == 0U) {
                reported_ipv4 = 0U;
            }
        }
        if (realtest_network_required && !realtest_success_reported &&
            realtest_network_deadline != 0U &&
            realtest_network_deadline != UINT64_MAX &&
            (int64_t)(x86_read_tsc() - realtest_network_deadline) >= 0) {
            realtest_checkpoint("OS_REALTEST_DHCP_TIMEOUT");
            realtest_finish_failure();
        }
        user_init_poll();
    }
}

__attribute__((noreturn)) void kernel_init_runtime_start(
    OS_BOOT_INFO *info, UINT64 framebuffer_virtual_base) {
    if (info == 0 ||
        info->BootstrapStackBase == 0U ||
        info->BootstrapStackSize != OS_BOOTSTRAP_STACK_SIZE ||
        info->BootstrapStackTop !=
            info->BootstrapStackBase + info->BootstrapStackSize) {
        kernel_serial_write("OS_KERNEL_STACK_FAIL\r\n");
        halt_forever();
    }
    kernel_serial_write("OS_KERNEL_STACK_OK\r\n");

    UINT64 boot_info_physical = info->BootInfoPhysicalBase != 0U ?
                                info->BootInfoPhysicalBase :
                                (UINT64)(uintptr_t)info;
    UINT64 direct_span = X86_64_DIRECT_MAP_END - X86_64_DIRECT_MAP_BASE + 1ULL;
    if (boot_info_physical >= direct_span ||
        sizeof(*info) > direct_span - boot_info_physical) {
        kernel_serial_write("OS_HIGH_BOOTINFO_FAIL\r\n");
        halt_forever();
    }
    OS_BOOT_INFO *high_info =
        (OS_BOOT_INFO *)phys_to_direct(paddr_make(boot_info_physical));
    if (high_info == 0 || high_info->Magic != OS_BOOTINFO_MAGIC) {
        kernel_serial_write("OS_HIGH_BOOTINFO_FAIL\r\n");
        halt_forever();
    }

    g_framebuffer_virtual_base = framebuffer_virtual_base;
    UINT64 high_stack_top = X86_64_DIRECT_MAP_BASE +
                            info->BootstrapStackBase +
                            info->BootstrapStackSize;
#if OS_REALTEST
    {
        paddr_t translated_stack = paddr_make(UINT64_MAX);
        paddr_t translated_entry = paddr_make(UINT64_MAX);
        UINT64 stack_probe = high_stack_top - 64U;
        UINT64 expected_stack = info->BootstrapStackBase +
                                info->BootstrapStackSize - 64U;
        UINT64 entry_address = (UINT64)(uintptr_t)kernel_runtime_main;
        UINT64 stack_flags = 0U;
        UINT64 entry_flags = 0U;
        kstatus_t stack_status = x86_translate_page(
            x86_current_root_table(), (vaddr_t)stack_probe,
            &translated_stack, &stack_flags);
        kstatus_t entry_status = x86_translate_page(
            x86_current_root_table(), (vaddr_t)entry_address,
            &translated_entry, &entry_flags);
        kernel_serial_printf_serial_only(
            "OS_STACK_SWITCH_PROBE stack_status=%d entry_status=%d "
            "stack_expected_lo=%u stack_expected_hi=%u stack_actual_lo=%u "
            "stack_actual_hi=%u stack_flags_lo=%u stack_flags_hi=%u "
            "entry_expected_lo=%u entry_expected_hi=%u entry_actual_lo=%u "
            "entry_actual_hi=%u entry_flags_lo=%u entry_flags_hi=%u\r\n",
            (int)stack_status, (int)entry_status,
            (UINT32)expected_stack, (UINT32)(expected_stack >> 32U),
            (UINT32)translated_stack.value,
            (UINT32)(translated_stack.value >> 32U),
            (UINT32)stack_flags, (UINT32)(stack_flags >> 32U),
            (UINT32)entry_address, (UINT32)(entry_address >> 32U),
            (UINT32)translated_entry.value,
            (UINT32)(translated_entry.value >> 32U),
            (UINT32)entry_flags, (UINT32)(entry_flags >> 32U));
        if (stack_status != K_OK || translated_stack.value != expected_stack ||
            (stack_flags & 3U) != 3U || entry_status != K_OK ||
            translated_entry.value != info->KernelPhysicalBase +
                                       (entry_address - info->KernelVirtualBase)) {
            realtest_capture("OS_STACK_SWITCH_PROBE_FAIL\r\n");
            realtest_finish_failure();
        }
        volatile UINT64 *stack_word = (volatile UINT64 *)(uintptr_t)stack_probe;
        UINT64 saved_word = *stack_word;
        *stack_word = 0x5A17C0DE5A17C0DEULL;
        bool write_ok = *stack_word == 0x5A17C0DE5A17C0DEULL;
        *stack_word = saved_word;
        if (!write_ok) {
            realtest_capture("OS_STACK_SWITCH_WRITE_FAIL\r\n");
            realtest_finish_failure();
        }
        realtest_checkpoint("RUNTIME_STACK_SWITCH_MEMORY_OK");
    }
#endif
    realtest_checkpoint("RUNTIME_STACK_SWITCH_BEGIN");
    realtest_checkpoint("RUNTIME_STACK_SWITCH_READY");
    x86_switch_stack_and_call(high_stack_top, kernel_runtime_main, high_info);
}
