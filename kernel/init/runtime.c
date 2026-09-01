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
#include <kernel/init_user_services.h>
#include <kernel/wait.h>
#include <kernel/vm.h>
#include <kernel/vfs.h>
#include <kernel/journal.h>
#include <kernel/litefs.h>
#include <kernel/e1000.h>
#include <kernel/net_core.h>
#include <kernel/net_manager.h>
#include <kernel/socket.h>
#include <kernel/console.h>
#include <kernel/irq.h>
#include <kernel/nvme_core.h>
#include <ascii_font.h>
#include <arch/x86_64/syscall_internal.h>
#include <usb/storage.h>
#include <kernel/sched.h>
#include <kernel/debug_stage.h>
#include <kernel/console_backend.h>

#ifndef LITEOS_DEBUG_SERIAL
#define LITEOS_DEBUG_SERIAL 0
#endif

/* REFACTOR_P3_RUNTIME_OWNER: post-init high-half continuation. */
static UINT64 g_framebuffer_virtual_base;

#define halt_forever() liteos_kernel_halt_forever_at(__FILE__, __LINE__)

static void __attribute__((noreturn)) kernel_runtime_main(void *context) {
    LITEOS_BOOT_INFO *info = (LITEOS_BOOT_INFO *)context;
    UINT64 benchmark_start;
    UINT64 direct_span = X86_64_DIRECT_MAP_END - X86_64_DIRECT_MAP_BASE + 1ULL;
    if (info == 0 || info->BootstrapStackBase >= direct_span ||
        info->BootstrapStackSize > direct_span - info->BootstrapStackBase) {
        liteos_serial_write("LITEOS_HIGH_STACK_FAIL\r\n");
        halt_forever();
    }

    UINT64 high_stack_base = X86_64_DIRECT_MAP_BASE + info->BootstrapStackBase;
    if (!liteos_arch_set_kernel_stack(high_stack_base, info->BootstrapStackSize) ||
        !sched_set_boot_kernel_stack(x86_tss_get_rsp0())) {
        liteos_serial_write("LITEOS_HIGH_STACK_FAIL\r\n");
        halt_forever();
    }
    x86_syscall_set_kernel_stack(high_stack_base + info->BootstrapStackSize);

    if ((info->Flags & LITEOS_BOOTINFO_HAS_FRAMEBUFFER) == 0 ||
        info->FrameBufferWidth == 0 || info->FrameBufferHeight == 0 ||
        info->FrameBufferPixelsPerScanLine < info->FrameBufferWidth) {
        liteos_serial_write("LITEOS_FRAMEBUFFER_MAP_FAIL\r\n");
        halt_forever();
    }
    UINT64 required_pixels = (UINT64)info->FrameBufferPixelsPerScanLine *
                             info->FrameBufferHeight;
    if (required_pixels > UINT64_MAX / sizeof(UINT32) ||
        required_pixels * sizeof(UINT32) > info->FrameBufferSize) {
        liteos_serial_write("LITEOS_FRAMEBUFFER_MAP_FAIL\r\n");
        halt_forever();
    }

    UINT64 framebuffer_virtual = g_framebuffer_virtual_base;
    if (framebuffer_virtual == 0U &&
        !liteos_map_framebuffer_wc(info, &framebuffer_virtual)) {
        liteos_serial_write("LITEOS_FRAMEBUFFER_MAP_FAIL\r\n");
        halt_forever();
    }
    /* The GOP terminal is an early-boot diagnostic surface only.  Do not
     * recreate it here: the framebuffer is now owned by display/window code,
     * and the physical realtest path records diagnostics on the FAT volume. */
    liteos_console_disable();
    liteos_serial_write_serial_only("LITEOS_GOP_CONSOLE_DISABLED\r\n");
    LITEOS_BOOT_INFO display_info;
    for (UINTN byte = 0; byte < sizeof(display_info); ++byte) {
        ((UINT8 *)&display_info)[byte] = ((const UINT8 *)info)[byte];
    }
    display_info.FrameBufferBase = framebuffer_virtual;

    if (!display_core_init(display_info.FrameBufferBase, display_info.FrameBufferSize,
                           display_info.FrameBufferWidth, display_info.FrameBufferHeight,
                           display_info.FrameBufferPixelsPerScanLine,
                           display_info.FrameBufferFormat, display_info.FrameBufferMask)) {
        liteos_serial_write("LITEOS_DISPLAY_REMAP_FAIL\r\n");
        halt_forever();
    }
    if (!display_core_self_test()) {
        liteos_serial_write("LITEOS_DISPLAY_CORE_FAIL\r\n");
        halt_forever();
    }
    liteos_serial_write("LITEOS_DISPLAY_CORE_OK\r\n");
    liteos_debug_stage(LITEOS_DEBUG_PHASE_DISPLAY,
                       LITEOS_DEBUG_STEP_READY, 1U);
    liteos_debug_stage_ready(LITEOS_DEBUG_PHASE_SPEC_13);

    /*
     * Optional QEMU Standard VGA page-flip backend. Physical machines simply
     * miss PCI 1234:1111 and keep the existing GOP path.
     */
    if (qemu_stdvga_flip_init(
            info->FrameBufferBase,
            framebuffer_virtual,
            info->FrameBufferSize)) {

        liteos_serial_write("LITEOS_QEMU_STDVGA_FLIP_OK\r\n");

    } else if (qemu_stdvga_hardware_present()) {
        liteos_serial_write("LITEOS_QEMU_STDVGA_FLIP_DISABLED ERR=");
        liteos_serial_write_u32(qemu_stdvga_last_error());
        liteos_serial_write("\r\n");
    }

    liteos_realtest_disable_runtime_services();
    if (!liteos_drop_identity_mapping()) {
        liteos_serial_write("LITEOS_IDENTITY_REMOVE_FAIL\r\n");
        halt_forever();
    }
    paddr_t unexpected_mapping;
    if (x86_translate_page(x86_current_root_table(), 0, &unexpected_mapping, 0) != K_ENOENT) {
        liteos_serial_write("LITEOS_IDENTITY_REMOVE_FAIL\r\n");
        halt_forever();
    }
    liteos_serial_write("LITEOS_IDENTITY_REMOVED_OK\r\n");

    /* Keep the framebuffer probe serial-only. */
    liteos_serial_write("LITEOS_FRAMEBUFFER_WC_OK\r\n");
    liteos_init_userspace_hooks_t runtime_userspace_hooks = {
        .write = liteos_serial_write,
        .write_u32 = liteos_serial_write_u32,
        .halt = liteos_kernel_halt_forever,
    };
    liteos_userspace_run_runtime_self_test(&runtime_userspace_hooks);
    liteos_serial_write("LITEOS_DISPLAY_COMMIT_OK\r\n");
    if (!x86_smp_remote_user_self_test()) {
        liteos_serial_write("LITEOS_SMP_USER_DISPATCH_FAIL\r\n");
        halt_forever();
    }
    liteos_serial_write("LITEOS_SMP_USER_DISPATCH_OK\r\n");
    if (!window_server_kernel_ready()) {
        liteos_serial_write("LITEOS_WINDOW_SERVER_KERNEL_FAIL\r\n");
        halt_forever();
    }
    liteos_serial_write("LITEOS_WINDOW_SERVER_KERNEL_OK\r\n");
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_7A,
                       LITEOS_DEBUG_STEP_ENTER, 0U);
    benchmark_start = telemetry_timestamp();
    if (!window_lifecycle_self_test()) {
        liteos_serial_write("LITEOS_WINDOW_TEST_FAIL\r\n");
        halt_forever();
    }
    kernel_perf_emit_scope("graphics.window", benchmark_start);
    liteos_serial_write("LITEOS_WINDOW_OK\r\n");
    liteos_init_userspace_hooks_t deferred_userspace_hooks = {
        .write = liteos_serial_write,
        .write_u32 = liteos_serial_write_u32,
        .halt = liteos_kernel_halt_forever,
    };
    if (x86_smp_discovered_count() <= 1U &&
        !liteos_userspace_start_window_worker(&deferred_userspace_hooks)) {
        halt_forever();
    }
    /*
     * Keep the optional artwork read out of the boot-critical hand-off.  On
     * a one-CPU machine the worker shares the BSP with the UAS/VFS path and
     * can take an unbounded amount of time from the runtime test's point of
     * view.  The compositor already has a solid hold surface, so publish the
     * desktop-running result first and start artwork loading afterwards.
     */
    liteos_console_disable();
    if (liteos_realtest_enabled()) liteos_realtest_finish_success();
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
        __asm__ volatile ("sti; hlt" : : : "memory");
        /*
         * The BSP remains on this bootstrap continuation after the normal
         * scheduler starts, while its queue.current is still the logical
         * idle thread.  AP idle loops consume reschedule IPIs themselves;
         * poll the same request here so a READY deferred worker cannot wait
         * for an unrelated timer tick on the BSP.
         */
        (void)x86_smp_take_reschedule_request();
        if (sched_runnable_count() != 0U) {
            (void)sched_try_run_ready();
        }
        /* Persist post-desktop HID/window diagnostics without doing any I/O
         * when the real-test buffer has not changed. */
        if (liteos_realtest_enabled()) (void)liteos_realtest_flush();
        net_manager_poll();
        if (net_manager_get_status(&net_status)) {
            /* 链路状态由 net_manager 统一采样。 */
            if (net_status.hardware_present) {
                if (reported_link_valid && reported_link_up != net_status.link_up) {
                    liteos_serial_write(net_status.link_up ?
                                        "LITEOS_NET_LINK_UP\r\n" :
                                        "LITEOS_NET_LINK_DOWN\r\n");
                }
                reported_link_valid = true;
                reported_link_up = net_status.link_up;
            } else {
                reported_link_valid = false;
            }
            if (net_status.ipv4_address != 0U &&
                net_status.ipv4_prefix_length != 0U &&
                reported_ipv4 == 0U) {
                liteos_serial_write("LITEOS_NET_DHCP_OK ");
                liteos_serial_write_u32(net_status.ipv4_address);
                liteos_serial_write("/");
                liteos_serial_write_u32(net_status.ipv4_prefix_length);
                liteos_serial_write("\r\n");
                reported_ipv4 = net_status.ipv4_address;
            } else if (net_status.ipv4_address == 0U) {
                reported_ipv4 = 0U;
            }
        }
        user_init_poll();
    }
}

__attribute__((noreturn)) void liteos_init_runtime_start(
    LITEOS_BOOT_INFO *info, UINT64 framebuffer_virtual_base) {
    if (info == 0 ||
        info->BootstrapStackBase == 0U ||
        info->BootstrapStackSize != LITEOS_BOOTSTRAP_STACK_SIZE ||
        info->BootstrapStackTop !=
            info->BootstrapStackBase + info->BootstrapStackSize) {
        liteos_serial_write("LITEOS_KERNEL_STACK_FAIL\r\n");
        halt_forever();
    }
    liteos_serial_write("LITEOS_KERNEL_STACK_OK\r\n");

    UINT64 boot_info_physical = info->BootInfoPhysicalBase != 0U ?
                                info->BootInfoPhysicalBase :
                                (UINT64)(uintptr_t)info;
    UINT64 direct_span = X86_64_DIRECT_MAP_END - X86_64_DIRECT_MAP_BASE + 1ULL;
    if (boot_info_physical >= direct_span ||
        sizeof(*info) > direct_span - boot_info_physical) {
        liteos_serial_write("LITEOS_HIGH_BOOTINFO_FAIL\r\n");
        halt_forever();
    }
    LITEOS_BOOT_INFO *high_info =
        (LITEOS_BOOT_INFO *)phys_to_direct(paddr_make(boot_info_physical));
    if (high_info == 0 || high_info->Magic != LITEOS_BOOTINFO_MAGIC) {
        liteos_serial_write("LITEOS_HIGH_BOOTINFO_FAIL\r\n");
        halt_forever();
    }

    g_framebuffer_virtual_base = framebuffer_virtual_base;
    x86_rebase_stack_and_call(X86_64_DIRECT_MAP_BASE,
                               kernel_runtime_main, high_info);
}
