#include <kernel/console.h>
#include <kernel/deferred.h>
#include <kernel/xhci.h>
#include "internal.h"

/* REFACTOR_P8_XHCI_RUNTIME_OWNER: runtime event draining, deferred-work
 * coalescing, and the post-event interrupter diagnostics. */
/* REFACTOR_P8_XHCI_RUNTIME_STATE_OWNER: runtime readiness is private to this
 * unit; controller self-tests and event consumers use the narrow API below. */

static bool g_xhci_runtime_ready;
static uint32_t g_xhci_runtime_event_batches;
static atomic_bool g_xhci_work_queued;

bool xhci_runtime_ready(void) {
    return g_xhci_runtime_ready;
}

void xhci_runtime_set_ready(bool ready) {
    g_xhci_runtime_ready = ready;
}

void xhci_runtime_reset_state(void) {
    g_xhci_runtime_ready = false;
    g_xhci_runtime_event_batches = 0U;
    atomic_init(&g_xhci_work_queued, false);
    xhci_interrupt_reset_state();
}

void xhci_runtime_stop(void) {
    g_xhci_runtime_ready = false;
    g_xhci_runtime_event_batches = 0U;
}

static bool xhci_process_primary_events(uint32_t budget) {
    xhci_state_t *state = xhci_controller_state();
    bool consumed;

    if (state == 0 || !state->initialized || !xhci_event_try_lock(state)) {
        return false;
    }
    consumed = xhci_process_event_ring(state, budget,
                                       &g_xhci_event_dispatch_ops);
    xhci_event_handler_complete(state);
    xhci_event_unlock(state);
    return consumed;
}

static bool xhci_process_aux_hid_events(uint32_t budget) {
    xhci_state_t *state = xhci_hid_controller_state();
    bool consumed = false;

    if (!xhci_hid_controller_active() || state == 0 || !state->initialized ||
        !xhci_event_try_lock(state)) {
        return false;
    }
    for (uint32_t index = 0U; index < budget; ++index) {
        xhci_trb_t event;
        uint32_t type;

        if (!xhci_next_event(state, &event)) break;
        type = (event.control >> XHCI_TRB_TYPE_SHIFT) & 0x3FU;
        if (type == XHCI_TRANSFER_EVENT_TYPE &&
            xhci_handle_hid_transfer_event(state, &event)) {
            consumed = true;
        }
    }
    xhci_event_handler_complete(state);
    xhci_event_unlock(state);
    return consumed;
}

bool xhci_process_events(uint32_t budget) {
    bool primary_consumed;
    bool aux_consumed;

    if (budget == 0U) budget = 1U;
    if (!g_xhci_runtime_ready) return false;
    ++g_xhci_runtime_event_batches;
    if (g_xhci_runtime_event_batches == 1U) {
        liteos_serial_write_serial_only("LITEOS_USB_RUNTIME_IRQ_OK\r\n");
    }
    /* Both independent xHCI event rings are drained by the same coalesced
     * worker, but each ring keeps its own lock and ERDP state. */
    primary_consumed = xhci_process_primary_events(budget);
    aux_consumed = xhci_process_aux_hid_events(budget);
    return primary_consumed || aux_consumed;
}

bool xhci_schedule_deferred_work(void) {
    bool expected = false;
    if (!g_xhci_runtime_ready) {
        return false;
    }
    if (!atomic_compare_exchange_strong_explicit(&g_xhci_work_queued, &expected,
                                                 true, memory_order_acq_rel,
                                                 memory_order_acquire)) {
        return false;
    }
    /* xHCI is the owner of the reserved critical slot.  HID and Hub events
     * must not wait behind unrelated normal deferred work; the normal queue
     * remains a bounded fallback for the rare critical-slot race. */
    if (deferred_schedule_critical(xhci_deferred_work, 0)) {
        return true;
    }
    if (deferred_try_schedule(xhci_deferred_work, 0)) {
        return true;
    }
    atomic_store_explicit(&g_xhci_work_queued, false,
                          memory_order_release);
    return false;
}

void xhci_deferred_work(void *argument) {
    bool consumed = false;
    bool pending = false;
    bool raced_irq = false;
    uint32_t pass = 0U;
    (void)argument;
    /* Consume the interrupt which caused this work item before draining the
     * ring.  The previous code tested this same bit only at the end, so the
     * interrupt that scheduled the item was mistaken for a new interrupt and
     * the worker continuously re-queued itself even with an empty ring. */
    (void)xhci_interrupt_take_pending();
    do {
        consumed = xhci_process_events(64U);
        ++pass;
    } while (consumed && pass < 8U);
    xhci_report_hid_completion_milestones();

    /* Release queue ownership before the final IRQ handshake.  An MSI-X can
     * arrive while this worker is draining the ring; the worker inherits that
     * coalesced IRQ before deciding whether to schedule another bounded pass. */
    atomic_store_explicit(&g_xhci_work_queued, false,
                          memory_order_release);

    pending = xhci_event_pending(xhci_controller_state());
    if (xhci_hid_controller_active() &&
        xhci_event_pending(xhci_hid_controller_state())) {
        pending = true;
    }
    raced_irq = xhci_interrupt_take_pending();

    if (pending || raced_irq) {
        (void)xhci_schedule_deferred_work();
    }
}
