#include <kernel/console.h>
#include <kernel/realtest.h>
#include <kernel/xhci.h>
#include "internal.h"

/* REFACTOR_P8_XHCI_SELF_TEST_OWNER: controller and runtime validation. */
static bool xhci_hardware_self_test_one(const pci_device_t *pci) {
    /* V3.10.3D1 SELF TEST ACTIVATION INLINED
     * No self-test activation helper remained after V3.10.3C.
     */

    xhci_clear_device_flags();
    xhci_runtime_reset_state();
    xhci_hid_runtime_initialize();
    xhci_hid_init();
    if (pci == 0) return false;
    xhci_clear_error();
    if (!xhci_core_initialize(xhci_controller_state(), pci)) return false;
    uint8_t selected_slot = 0U;
    bool has_device =
        xhci_probe_connected_ports(
            xhci_controller_state(),
            &selected_slot);
    if (!has_device && xhci_last_error() != 0U) {
        xhci_core_destroy(xhci_controller_state());
        return false;
    }
    uint8_t slot = 0;
    bool keep_controller = false;
    bool success;
    if(has_device)
    {
        success =
            false;

        if(selected_slot != 0U)
        {
            xhci_slot_device_t *slot_dev =
                xhci_topology_slot(selected_slot);

            if(slot_dev->used &&
               slot_dev->context.device_slot ==
                   selected_slot)
            {
                /*
                 * V3.10.6B8C DIRECT SELF-TEST SLOT
                 *
                 * selected_slot already names the permanent
                 * canonical context.
                 */
                xhci_recompute_topology(
                    xhci_controller_state());

                success =
                    true;

                /* A successful probe already established the production
                 * controller state.  Resetting and re-enumerating the same
                 * device here invalidates freshly configured UAS endpoints
                 * before the first SCSI command.  Keep the stable Slot; the
                 * explicit re-enumeration helper remains available to tests
                 * which deliberately exercise detach/attach lifetime. */
                keep_controller =
                    true;
                liteos_realtest_mark("XHCI_STABLE_SLOT_RETAINED");
            }
        }

        if(!success)
        {
            xhci_set_error(69U);
        }
    } else {
        success = xhci_submit_command(xhci_controller_state(), XHCI_COMMAND_RING_TYPE, 0, 0, &slot) &&
                  slot != 0U;
    }
    uint32_t saved_error = success ? 0U : xhci_last_error();
    if (success && keep_controller) {
        xhci_recompute_topology(xhci_controller_state());

        /*
         * Startup Hub status TDs are deliberately armed before runtime.
         * Drain every setup-time completion now, while MSI-X is still hidden
         * from runtime consumers, then normalize every Hub to exactly one
         * pending status TD.
         */
        if(!xhci_drain_startup_events(
               xhci_controller_state(),
               &g_xhci_event_dispatch_ops) ||
           !xhci_hub_runtime_start(
               xhci_controller_state(),
               &g_xhci_hub_runtime_ops))
        {
            if(xhci_last_error() == 0U)
            {
                xhci_set_error(76U);
            }

            liteos_serial_write(
                "LITEOS_XHCI_HUB_RUNTIME_FAIL\r\n");

            success =
                false;

            keep_controller =
                false;
        }

    /* Runtime xHCI I/O is interrupt-driven.  A controller without an MSI or
     * MSI-X route is recorded as unavailable instead of being polled. */
    xhci_runtime_set_ready(success);
    if (success && !xhci_core_bind_msix(xhci_controller_state())) {
            uint32_t irq_error = xhci_last_error();
            uint32_t irq_stage = xhci_msix_failure_stage();
            liteos_realtest_mark_number("XHCI_PRIMARY_MSIX_ERROR",
                                        irq_error);
            liteos_realtest_mark_number("XHCI_PRIMARY_MSIX_STAGE",
                                        irq_stage);
            liteos_serial_write("LITEOS_XHCI_MSIX_FAIL_STAGE=");
            liteos_serial_write_u32(irq_stage);
            liteos_serial_write("\r\n");
            success = false;
            keep_controller = false;
            xhci_runtime_set_ready(false);
            liteos_realtest_mark_number("XHCI_PRIMARY_IRQ_MODE", 0U);
            liteos_serial_write("LITEOS_XHCI_IRQ_UNAVAILABLE\r\n");
        } else if(success) {
            liteos_realtest_mark_number("XHCI_PRIMARY_IRQ_MODE", 1U);
            liteos_serial_write("LITEOS_XHCI_MSIX_OK\r\n");
            liteos_serial_write(
                "LITEOS_XHCI_HUB_RUNTIME_OK\r\n");

            /* Count only post-MSI-X runtime HID completions. */
            xhci_hid_runtime_reset_completion_counters();

            /*
             * A completion may have landed in the tiny bind window after the
             * startup drain and before MSI-X was unmasked.  Schedule one
             * bounded deferred pass so such an event cannot be stranded.
             */
            (void)xhci_schedule_deferred_work();
        }
    }
    if (slot != 0U && !keep_controller &&
        !xhci_submit_command(xhci_controller_state(), XHCI_DISABLE_SLOT_TYPE, slot, 0, 0)) {
        if (success) saved_error = xhci_last_error();
        success = false;
    }
    if (!success && saved_error == 0U) saved_error = xhci_last_error() != 0U ? xhci_last_error() : 23U;
    if (!success) xhci_set_error(saved_error);
    if (!success) xhci_runtime_set_ready(false);
    if (!success || !keep_controller) xhci_core_destroy(xhci_controller_state());
    return success;
}

static bool xhci_start_auxiliary_hid(const pci_device_t *pci) {
    xhci_state_t *state = xhci_hid_controller_state();

    if (state == 0 || pci == 0 || state->initialized) return false;
    xhci_hid_controller_set_active(false);
    xhci_clear_error();
    liteos_realtest_mark_number("XHCI_AUX_DEVICE", pci->device_id);
    liteos_realtest_mark_number("XHCI_AUX_BUS", pci->bus);
    liteos_realtest_mark_number("XHCI_AUX_PCI_STATUS", pci->status);
    liteos_realtest_mark_number("XHCI_AUX_PCI_COMMAND", pci->command);
    liteos_realtest_mark_number("XHCI_AUX_MSI_CAP", pci->msi_capability);
    liteos_realtest_mark_number("XHCI_AUX_MSIX_CAP", pci->msix_capability);
    liteos_realtest_mark_number("XHCI_AUX_MSIX_ENTRIES",
                                pci->msix_table_size);
    liteos_realtest_mark_number("XHCI_AUX_MSIX_BAR", pci->msix_table_bar);
    liteos_realtest_mark_number("XHCI_AUX_MSIX_OFFSET",
                                pci->msix_table_offset);
    if (pci->msix_table_bar < PCI_MAX_BARS) {
        liteos_realtest_mark_number("XHCI_AUX_MSIX_BAR_FLAGS",
                                    pci->bars[pci->msix_table_bar].flags);
        liteos_realtest_mark_number("XHCI_AUX_MSIX_BAR_LENGTH_LO",
                                    (uint32_t)pci->bars[pci->msix_table_bar].length);
    }
    if (!xhci_core_initialize(state, pci)) {
        liteos_realtest_mark_number("XHCI_AUX_INIT_ERROR", xhci_last_error());
        liteos_serial_write("LITEOS_XHCI_AUX_HID_FAIL\r\n");
        return false;
    }
    if (!xhci_probe_hid_controller(state)) {
        liteos_realtest_mark_number("XHCI_AUX_PROBE_ERROR", xhci_last_error());
        liteos_serial_write("LITEOS_XHCI_AUX_HID_FAIL\r\n");
        (void)xhci_core_destroy(state);
        return false;
    }
    if (!xhci_core_bind_msix(state)) {
        uint32_t irq_stage = xhci_msix_failure_stage();
        liteos_realtest_mark_number("XHCI_AUX_MSIX_ERROR", xhci_last_error());
        liteos_realtest_mark_number("XHCI_AUX_MSIX_STAGE",
                                    irq_stage);
        /* Enumeration and endpoint setup are not enough for runtime HID:
         * without an interrupt route there is no event-driven completion
         * path, so release this controller and leave a failure marker. */
        xhci_hid_controller_set_active(false);
        liteos_realtest_mark_number("XHCI_AUX_HID_IRQ_MODE", 0U);
        liteos_realtest_mark("XHCI_AUX_HID_IRQ_UNAVAILABLE");
        liteos_serial_write("LITEOS_XHCI_AUX_HID_IRQ_UNAVAILABLE\r\n");
        (void)xhci_core_destroy(state);
        return false;
    }
    xhci_hid_controller_set_active(true);
    liteos_realtest_mark_number("XHCI_AUX_HID_IRQ_MODE", 1U);
    liteos_realtest_mark("XHCI_AUX_HID_READY");
    liteos_serial_write("LITEOS_XHCI_AUX_HID_MSIX_OK\r\n");
    (void)xhci_schedule_deferred_work();
    return true;
}

static void xhci_start_auxiliary_hid_controllers(uint32_t primary_index) {
    uint32_t count = xhci_pci_controller_count();

    for (uint32_t index = 0U; index < count; ++index) {
        const pci_device_t *controller;

        if (index == primary_index) continue;
        controller = xhci_pci_controller_at(index);
        liteos_realtest_mark_number("XHCI_AUX_CONTROLLER_BEGIN", index);
        if (xhci_start_auxiliary_hid(controller)) {
            liteos_realtest_mark_number("XHCI_AUX_CONTROLLER_READY", index);
            return;
        }
    }
}

bool xhci_hardware_self_test(void) {
    uint32_t controller_count = xhci_pci_controller_count();
    const pci_device_t *fallback_controller = 0;
    xhci_set_hardware_present(controller_count != 0U);
    if (controller_count == 0U) return true;
    for (uint32_t index = 0U; index < controller_count; ++index) {
        const pci_device_t *controller = xhci_pci_controller_at(index);
        liteos_realtest_mark_number("XHCI_CONTROLLER_BEGIN", index);
        if (controller != 0) {
            liteos_realtest_mark_number("XHCI_CONTROLLER_DEVICE",
                                        controller->device_id);
            liteos_realtest_mark_number("XHCI_CONTROLLER_BUS",
                                        controller->bus);
        }
        if (!xhci_hardware_self_test_one(controller)) {
            liteos_realtest_mark_number("XHCI_CONTROLLER_ERROR",
                                        xhci_last_error());
            continue;
        }
        /* An earlier controller may have produced a detailed EP0 failure.
         * That failure is no longer boot-fatal once a later controller owns
         * the usable USB topology. */
        liteos_realtest_clear_failure_state();

        /* A passing controller is not enough for a real boot: the FAT volume
         * must be reachable through its MSC device. Prefer that controller
         * when firmware exposes several independent xHCI functions. */
        if (xhci_usb_mass_storage_configured()) {
            liteos_realtest_mark_number("XHCI_CONTROLLER_MSC", index);
            xhci_start_auxiliary_hid_controllers(index);
            return true;
        }
        liteos_realtest_mark_number("XHCI_CONTROLLER_NO_MSC", index);
        fallback_controller = controller;
        if (index + 1U < controller_count) {
            (void)xhci_core_destroy(xhci_controller_state());
        } else {
            /* The last controller is usable for HID even when no USB mass
             * storage device is attached.  It is already initialized; do
             * not run the self-test a second time on the live controller. */
            liteos_realtest_mark("XHCI_FALLBACK_READY");
            return true;
        }
    }

    /* Keep a usable HID-only controller as a last resort when no controller
     * owns mass storage. Re-run it because probing another controller tears
     * down the shared xHCI state. */
    if (fallback_controller != 0 &&
        xhci_hardware_self_test_one(fallback_controller)) {
        liteos_realtest_clear_failure_state();
        liteos_realtest_mark("XHCI_FALLBACK_READY");
        return true;
    }
    return false;
}
