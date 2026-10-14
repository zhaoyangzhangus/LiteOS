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

void __attribute__((noreturn)) kernel_halt_forever(void) {
    realtest_record_failure(kernel_debug_stage_file(),
                                   kernel_debug_stage_line());
    (void)realtest_flush();
    if (realtest_enabled()) realtest_finish_failure();
    kernel_halt();
}

/* Keep the direct boot-entry failure tied to an exact source location. */
void __attribute__((noreturn)) kernel_halt_forever_at(const char *file,
                                                            uint32_t line) {
    realtest_record_failure(file, line);
    uint16_t phase = kernel_debug_stage_phase();
    if (phase == 0U) phase = OS_DEBUG_PHASE_BOOT;
    kernel_debug_stage_fail_at(phase, OS_DEBUG_STEP_FAIL, K_EIO,
                               file, line);
    (void)realtest_flush();
    if (realtest_enabled()) realtest_finish_failure();
    kernel_halt();
}

#define halt_forever() kernel_halt_forever_at(__FILE__, __LINE__)

#if OS_REALTEST
static bool realtest_descriptor_intersects(const EFI_MEMORY_DESCRIPTOR *descriptor,
                                           UINT64 start, UINT64 size) {
    UINT64 descriptor_bytes;
    UINT64 descriptor_end;
    UINT64 range_end;

    if (descriptor == 0 || size == 0 ||
        descriptor->NumberOfPages > UINT64_MAX / PAGE_SIZE ||
        start > UINT64_MAX - size) return false;
    descriptor_bytes = descriptor->NumberOfPages * PAGE_SIZE;
    if (descriptor->PhysicalStart > UINT64_MAX - descriptor_bytes) return false;
    descriptor_end = descriptor->PhysicalStart + descriptor_bytes;
    range_end = start + size;
    return start < descriptor_end && descriptor->PhysicalStart < range_end;
}

static void realtest_log_boot_layout(const OS_BOOT_INFO *info) {
    const UINT8 *map;
    UINT64 descriptor_size;

    if (info == 0) return;
    kernel_serial_printf_serial_only(
        "OS_BOOT_LAYOUT kernel_lo=%u kernel_hi=%u kernel_size_lo=%u "
        "kernel_size_hi=%u stack_lo=%u stack_hi=%u stack_size=%u "
        "bootinfo_lo=%u bootinfo_hi=%u map_lo=%u map_hi=%u map_size=%u "
        "map_capacity=%u loader_lo=%u loader_hi=%u loader_size=%u\r\n",
        (UINT32)info->KernelPhysicalBase,
        (UINT32)(info->KernelPhysicalBase >> 32U),
        (UINT32)info->KernelSize,
        (UINT32)(info->KernelSize >> 32U),
        (UINT32)info->BootstrapStackBase,
        (UINT32)(info->BootstrapStackBase >> 32U),
        (UINT32)info->BootstrapStackSize,
        (UINT32)info->BootInfoPhysicalBase,
        (UINT32)(info->BootInfoPhysicalBase >> 32U),
        (UINT32)info->MemoryMap,
        (UINT32)(info->MemoryMap >> 32U),
        (UINT32)info->MemoryMapSize,
        (UINT32)info->MemoryMapBufferSize,
        (UINT32)info->LoaderImageBase,
        (UINT32)(info->LoaderImageBase >> 32U),
        (UINT32)info->LoaderImageSize);

    if (info->MemoryMap == 0 ||
        info->MemoryDescriptorSize < sizeof(EFI_MEMORY_DESCRIPTOR) ||
        info->MemoryMapSize < info->MemoryDescriptorSize) return;
    map = (const UINT8 *)(uintptr_t)info->MemoryMap;
    descriptor_size = info->MemoryDescriptorSize;
    for (UINT64 offset = 0;
         offset <= info->MemoryMapSize - descriptor_size;
         offset += descriptor_size) {
        const EFI_MEMORY_DESCRIPTOR *descriptor =
            (const EFI_MEMORY_DESCRIPTOR *)(map + offset);
        UINT32 targets = 0U;
        if (realtest_descriptor_intersects(descriptor,
                                           info->KernelPhysicalBase,
                                           info->KernelSize)) targets |= 1U;
        if (realtest_descriptor_intersects(descriptor,
                                           info->BootstrapStackBase,
                                           info->BootstrapStackSize)) targets |= 2U;
        if (realtest_descriptor_intersects(descriptor,
                                           info->BootInfoPhysicalBase,
                                           sizeof(*info))) targets |= 4U;
        if (realtest_descriptor_intersects(descriptor,
                                           info->MemoryMap,
                                           info->MemoryMapBufferSize != 0U ?
                                               info->MemoryMapBufferSize :
                                               info->MemoryMapSize)) targets |= 8U;
        if (realtest_descriptor_intersects(descriptor,
                                           info->LoaderImageBase,
                                           info->LoaderImageSize)) targets |= 16U;
        if (targets == 0U) continue;
        kernel_serial_printf_serial_only(
            "OS_BOOT_DESC target=%u type=%u start_lo=%u start_hi=%u "
            "pages_lo=%u pages_hi=%u attr_lo=%u attr_hi=%u\r\n",
            targets, descriptor->Type,
            (UINT32)descriptor->PhysicalStart,
            (UINT32)(descriptor->PhysicalStart >> 32U),
            (UINT32)descriptor->NumberOfPages,
            (UINT32)(descriptor->NumberOfPages >> 32U),
            (UINT32)descriptor->Attribute,
            (UINT32)(descriptor->Attribute >> 32U));
    }
}
#endif

/*
 * This function only orders the named initialization Owners.  Post-scheduler
 * self-tests and the high-half continuation live behind explicit boundaries
 * so a debugger can stop at the transition that owns the failure.
 */
void kernel_entry(OS_BOOT_INFO *info) {
    UINT64 framebuffer_virtual_base = 0U;

    realtest_boot_info(info);
#if OS_REALTEST
    realtest_log_boot_layout(info);
#endif
    realtest_mark("KERNEL_ENTRY");
    kernel_console_serial_init();
    kernel_serial_write_serial_only("OS_KERNEL_ENTRY\r\n");
    if (kernel_console_init_early(info)) {
        kernel_serial_write_serial_only("OS_EARLY_GOP_OK\r\n");
    }

    kernel_init_early_hooks_t early_hooks = {
        .write = kernel_serial_write_serial_only,
        .halt = kernel_halt_forever,
    };
    if (!kernel_init_early(info, &early_hooks)) halt_forever();
    realtest_mark("EARLY_OK");

    kernel_init_memory_hooks_t memory_hooks = {
        .write = kernel_serial_write_serial_only,
        .halt = kernel_halt_forever,
        .console_init = kernel_console_init,
        .console_disable = kernel_console_disable,
    };
    if (!kernel_init_memory(info, &memory_hooks,
                            &framebuffer_virtual_base)) halt_forever();
    realtest_mark("MEMORY_OK");

    kernel_init_devices_hooks_t device_hooks = {
        .write = kernel_serial_write_serial_only,
        .write_u32 = kernel_serial_write_u32_serial_only,
        .halt = kernel_halt_forever,
    };
    if (!kernel_init_devices(info, &device_hooks)) halt_forever();
    realtest_mark("DEVICES_OK");

    kernel_init_core_hooks_t core_hooks = {
        .write = kernel_serial_write_serial_only,
        .halt = kernel_halt_forever,
    };
    if (!kernel_init_core(&core_hooks)) halt_forever();
    realtest_mark("CORE_OK");

#if OS_REALTEST
    if (prepare_realtest_root(info)) {
        kernel_serial_write_serial_only("OS_REALTEST_EARLY_ROOT_OK\r\n");
        realtest_checkpoint("ROOT_PROBE_READY");
    } else {
        kernel_serial_write_serial_only("OS_REALTEST_EARLY_ROOT_FAIL\r\n");
        /* A real-hardware result is only valid when its trace is on the test
         * USB volume. Do not silently continue on the NVMe fallback, because
         * that would make the host copy the loader-only log from F:. */
        realtest_mark("FAILURE_ROOT_PROBE");
        realtest_finish_failure();
    }
#endif

    const nvme_controller_t *active_controller = 0;
    realtest_checkpoint("STORAGE_BEGIN");
    kernel_init_storage_hooks_t storage_hooks = {
        .write = kernel_serial_write_serial_only,
        .write_u32 = kernel_serial_write_u32_serial_only,
        .halt = kernel_halt_forever,
    };
    if (!kernel_init_storage(&storage_hooks, &active_controller)) {
        halt_forever();
    }
    realtest_mark("STORAGE_OK");
    realtest_checkpoint("STORAGE_OK");

    kernel_init_scheduler_hooks_t scheduler_hooks = {
        .write = kernel_serial_write_serial_only,
        .write_u32 = kernel_serial_write_u32_serial_only,
        .halt = kernel_halt_forever,
    };
    realtest_mark("SCHEDULER_BEGIN");
    realtest_checkpoint("SCHEDULER_BEGIN");
    if (!kernel_init_scheduler(info, &scheduler_hooks)) halt_forever();
    realtest_mark("SCHEDULER_OK");
    realtest_checkpoint("SCHEDULER_OK");

    realtest_mark("POST_SCHEDULER_BEGIN");
    /* From this boundary onward the framebuffer belongs to the desktop.
     * Diagnostics remain in the realtest capture, not in the GOP terminal. */
    kernel_console_disable();
    realtest_checkpoint("POST_SCHEDULER_BEGIN");
    if (!kernel_init_post_scheduler(info, active_controller)) {
        halt_forever();
    }
    realtest_mark("POST_SCHEDULER_OK");
    realtest_checkpoint("POST_SCHEDULER_OK");

    realtest_mark("RUNTIME_ENTRY");
    realtest_checkpoint("RUNTIME_BEGIN");
    kernel_init_runtime_start(info, framebuffer_virtual_base);
}
