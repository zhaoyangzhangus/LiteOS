#include <kernel/init_devices.h>

#include "arch/x86_64/acpi.h"
#include <kernel/block.h>
#include <kernel/debug_stage.h>
#include <kernel/input.h>
#include <kernel/iommu.h>
#include <kernel/pci.h>
#include <kernel/realtest.h>
#include <kernel/xhci.h>

static BOOLEAN devices_fail_at(const liteos_init_devices_hooks_t *hooks,
                               const CHAR8 *message, const char *file,
                               uint32_t line) {
    liteos_debug_stage_fail_at(LITEOS_DEBUG_PHASE_DRIVER,
                               LITEOS_DEBUG_STEP_FAIL, K_EIO, file, line);
    hooks->write(message);
    hooks->halt();
    return 0;
}

#define devices_fail(hooks, message) \
    devices_fail_at((hooks), (message), __FILE__, __LINE__)

BOOLEAN liteos_init_devices(LITEOS_BOOT_INFO *info,
                            const liteos_init_devices_hooks_t *hooks) {
    if (info == 0 || hooks == 0 || hooks->write == 0 ||
        hooks->write_u32 == 0 || hooks->halt == 0) return 0;

    if (!x86_acpi_discover(info) || x86_acpi_platform() == 0) {
        return devices_fail(hooks, "LITEOS_ACPI_FAIL\r\n");
    }
    hooks->write("LITEOS_ACPI_CPU_COUNT=");
    hooks->write_u32(x86_acpi_platform()->cpu_count);
    hooks->write("\r\nLITEOS_ACPI_OK\r\n");
    hooks->write(x86_acpi_sleep_supported() ?
                 "LITEOS_ACPI_SLEEP_OK\r\n" :
                 "LITEOS_ACPI_SLEEP_ABSENT\r\n");

    liteos_debug_stage_enter(LITEOS_DEBUG_PHASE_SPEC_9);
    if (!iommu_self_test()) {
        return devices_fail(hooks, "LITEOS_IOMMU_FAIL\r\n");
    }
    hooks->write(iommu_hardware_enabled() ?
                 "LITEOS_IOMMU_VTD_OK\r\n" :
                 "LITEOS_IOMMU_IDENTITY_FALLBACK\r\n");
    if (!pci_ecam_self_test()) {
        hooks->write("LITEOS_PCI_ECAM_FAIL=");
        hooks->write_u32(pci_ecam_last_error());
        return devices_fail(hooks, "\r\n");
    }
    hooks->write("LITEOS_PCI_ECAM_OK\r\n");

    liteos_debug_stage_enter(LITEOS_DEBUG_PHASE_SPEC_11);
    if (!input_core_init() || !input_core_self_test()) {
        return devices_fail(hooks, "LITEOS_INPUT_CORE_FAIL\r\n");
    }
    hooks->write("LITEOS_INPUT_CORE_OK\r\n");
    if (!xhci_hardware_self_test()) {
        hooks->write("LITEOS_XHCI_FAIL=");
        hooks->write_u32(xhci_last_error());
        liteos_realtest_mark_value("FAILURE_XHCI", xhci_last_error());
        return devices_fail(hooks, "\r\n");
    }
    /* xHCI status/error state is now isolated from controller/protocol units. */
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_8,
                       LITEOS_DEBUG_STEP_PROGRESS, 20U);
    /* Runtime readiness is private to xHCI runtime.c and is exposed only by
     * its narrow setter/getter contract. */
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_8,
                       LITEOS_DEBUG_STEP_PROGRESS, 21U);
    /* Controller halt/bring-up primitives remain private to lifecycle.c;
     * core.c only orchestrates the private-contract calls. */
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_8,
                       LITEOS_DEBUG_STEP_PROGRESS, 22U);
    /* IRQ pending-edge state is consumed only through interrupt.c's atomic
     * handoff, so the runtime worker cannot reach into ISR-owned storage. */
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_8,
                       LITEOS_DEBUG_STEP_PROGRESS, 23U);
    /* MSI-X failure diagnostics are read through interrupt.c's getter; the
     * self-test does not own or reach into the interrupt state. */
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_8,
                       LITEOS_DEBUG_STEP_PROGRESS, 24U);
    hooks->write(xhci_hardware_present() ?
                 "LITEOS_XHCI_HW_OK\r\n" :
                 "LITEOS_XHCI_ABSENT\r\n");
    if (xhci_hardware_present()) {
        hooks->write("LITEOS_USB_DEVICE_COUNT=");
        hooks->write_u32(xhci_usb_device_count());
        hooks->write("\r\n");
    }
    if (xhci_usb_device_enumerated()) hooks->write("LITEOS_USB_ENUM_OK\r\n");
    if (xhci_usb_hid_configured()) hooks->write("LITEOS_USB_HID_OK\r\n");
    if (xhci_usb_keyboard_configured()) {
        hooks->write("LITEOS_USB_KEYBOARD_OK\r\n");
    }
    if (xhci_usb_mouse_configured()) hooks->write("LITEOS_USB_MOUSE_OK\r\n");
    if (xhci_usb_bluetooth_configured()) {
        hooks->write("LITEOS_USB_BLUETOOTH_OK\r\n");
    }
    if (xhci_usb_audio_configured()) {
        hooks->write("LITEOS_USB_AUDIO_OK\r\n");
        hooks->write("LITEOS_USB_AUDIO_COMPLETED=");
        hooks->write_u32(xhci_usb_audio_completed());
        hooks->write("\r\n");
    }
    if (xhci_usb_hub_configured()) {
        hooks->write("LITEOS_USB_HUB_OK PORTS=");
        hooks->write_u32(xhci_usb_hub_port_count());
        hooks->write("\r\n");
    }
    if (xhci_usb_hub_downstream_configured()) {
        hooks->write("LITEOS_USB_HUB_DOWNSTREAM_OK\r\n");
    }
    liteos_debug_stage_ready(LITEOS_DEBUG_PHASE_SPEC_11);

    if (!block_multiqueue_self_test()) {
        return devices_fail(hooks, "LITEOS_BLOCK_MULTIQUEUE_FAIL\r\n");
    }
    hooks->write("LITEOS_BLOCK_MULTIQUEUE_OK\r\n");
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_3,
                       LITEOS_DEBUG_STEP_PROGRESS, 3U);
    return 1;
}
