#include <kernel/init_devices.h>

#include "arch/x86_64/acpi.h"
#include <kernel/block.h>
#include <kernel/debug_stage.h>
#include <kernel/input.h>
#include <kernel/iommu.h>
#include <kernel/pci.h>
#include <kernel/realtest.h>
#include <kernel/xhci.h>

#include "../drivers/gpu/nvidia/rm/include/nv_rm_driver.h"

#ifndef OS_NVIDIA_PHASE5
#define OS_NVIDIA_PHASE5 0
#endif

static BOOLEAN devices_fail_at(const kernel_init_devices_hooks_t *hooks,
                               const CHAR8 *message, const char *file,
                               uint32_t line) {
    kernel_debug_stage_fail_at(OS_DEBUG_PHASE_DRIVER,
                               OS_DEBUG_STEP_FAIL, K_EIO, file, line);
    hooks->write(message);
    hooks->halt();
    return 0;
}

#define devices_fail(hooks, message) \
    devices_fail_at((hooks), (message), __FILE__, __LINE__)

BOOLEAN kernel_init_devices(OS_BOOT_INFO *info,
                            const kernel_init_devices_hooks_t *hooks) {
    if (info == 0 || hooks == 0 || hooks->write == 0 ||
        hooks->write_u32 == 0 || hooks->halt == 0) return 0;

    if (!x86_acpi_discover(info) || x86_acpi_platform() == 0) {
        return devices_fail(hooks, "OS_ACPI_FAIL\r\n");
    }
    hooks->write("OS_ACPI_CPU_COUNT=");
    hooks->write_u32(x86_acpi_platform()->cpu_count);
    hooks->write("\r\nOS_ACPI_OK\r\n");
    hooks->write(x86_acpi_sleep_supported() ?
                 "OS_ACPI_SLEEP_OK\r\n" :
                 "OS_ACPI_SLEEP_ABSENT\r\n");

    kernel_debug_stage_enter(OS_DEBUG_PHASE_SPEC_9);
    if (!iommu_self_test()) {
        return devices_fail(hooks, "OS_IOMMU_FAIL\r\n");
    }
    hooks->write(iommu_hardware_enabled() ?
                 "OS_IOMMU_VTD_OK\r\n" :
                 "OS_IOMMU_IDENTITY_FALLBACK\r\n");
    if (!pci_ecam_self_test()) {
        hooks->write("OS_PCI_ECAM_FAIL=");
        hooks->write_u32(pci_ecam_last_error());
        return devices_fail(hooks, "\r\n");
    }
    hooks->write("OS_PCI_ECAM_OK\r\n");

    /* Phase 5 needs the mounted root filesystem for the matching GSP image;
     * the runtime continuation performs the same real probe after filesystem
     * initialization.  Earlier phases retain their original early probe. */
#if !OS_NVIDIA_PHASE5
    if (nv_rm_driver_probe() != K_OK)
        hooks->write("OS_NVIDIA_PROBE_FAIL\r\n");
#else
    hooks->write("OS_NVIDIA_PROBE_DEFERRED\r\n");
#endif

    kernel_debug_stage_enter(OS_DEBUG_PHASE_SPEC_11);
    if (!input_core_init() || !input_core_self_test()) {
        return devices_fail(hooks, "OS_INPUT_CORE_FAIL\r\n");
    }
    hooks->write("OS_INPUT_CORE_OK\r\n");
    if (!xhci_hardware_self_test()) {
        hooks->write("OS_XHCI_FAIL=");
        hooks->write_u32(xhci_last_error());
        realtest_mark_value("FAILURE_XHCI", xhci_last_error());
        return devices_fail(hooks, "\r\n");
    }
    /* xHCI status/error state is now isolated from controller/protocol units. */
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_8,
                       OS_DEBUG_STEP_PROGRESS, 20U);
    /* Runtime readiness is private to xHCI runtime.c and is exposed only by
     * its narrow setter/getter contract. */
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_8,
                       OS_DEBUG_STEP_PROGRESS, 21U);
    /* Controller halt/bring-up primitives remain private to lifecycle.c;
     * core.c only orchestrates the private-contract calls. */
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_8,
                       OS_DEBUG_STEP_PROGRESS, 22U);
    /* IRQ pending-edge state is consumed only through interrupt.c's atomic
     * handoff, so the runtime worker cannot reach into ISR-owned storage. */
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_8,
                       OS_DEBUG_STEP_PROGRESS, 23U);
    /* MSI-X failure diagnostics are read through interrupt.c's getter; the
     * self-test does not own or reach into the interrupt state. */
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_8,
                       OS_DEBUG_STEP_PROGRESS, 24U);
    hooks->write(xhci_hardware_present() ?
                 "OS_XHCI_HW_OK\r\n" :
                 "OS_XHCI_ABSENT\r\n");
    if (xhci_hardware_present()) {
        hooks->write("OS_USB_DEVICE_COUNT=");
        hooks->write_u32(xhci_usb_device_count());
        hooks->write("\r\n");
    }
    if (xhci_usb_device_enumerated()) hooks->write("OS_USB_ENUM_OK\r\n");
    if (xhci_usb_hid_configured()) hooks->write("OS_USB_HID_OK\r\n");
    if (xhci_usb_keyboard_configured()) {
        hooks->write("OS_USB_KEYBOARD_OK\r\n");
    }
    if (xhci_usb_mouse_configured()) hooks->write("OS_USB_MOUSE_OK\r\n");
    if (xhci_usb_bluetooth_configured()) {
        hooks->write("OS_USB_BLUETOOTH_OK\r\n");
    }
    if (xhci_usb_audio_configured()) {
        hooks->write("OS_USB_AUDIO_OK\r\n");
        hooks->write("OS_USB_AUDIO_COMPLETED=");
        hooks->write_u32(xhci_usb_audio_completed());
        hooks->write("\r\n");
    }
    if (xhci_usb_hub_configured()) {
        hooks->write("OS_USB_HUB_OK PORTS=");
        hooks->write_u32(xhci_usb_hub_port_count());
        hooks->write("\r\n");
    }
    if (xhci_usb_hub_downstream_configured()) {
        hooks->write("OS_USB_HUB_DOWNSTREAM_OK\r\n");
    }
    kernel_debug_stage_ready(OS_DEBUG_PHASE_SPEC_11);

    if (!block_multiqueue_self_test()) {
        return devices_fail(hooks, "OS_BLOCK_MULTIQUEUE_FAIL\r\n");
    }
    hooks->write("OS_BLOCK_MULTIQUEUE_OK\r\n");
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_3,
                       OS_DEBUG_STEP_PROGRESS, 3U);
    return 1;
}
