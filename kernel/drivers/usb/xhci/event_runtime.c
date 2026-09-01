#include "internal.h"

/* REFACTOR_P8_XHCI_EVENT_RUNTIME_OWNER: Event Ring consumption, deferred
 * event handoff, and interrupter completion are one low-level Owner. */

#define XHCI_RING_TRB_COUNT 256U
#define XHCI_TRB_CYCLE (1U << 0)
#define XHCI_RUNTIME_ERDP 0x18U
#define XHCI_RUNTIME_INTR0 0x20U
#define XHCI_ERDP_EHB (1ULL << 3)
#define XHCI_IMAN_IP (1U << 0)
#define XHCI_IMAN_IE (1U << 1)

bool xhci_event_pending(const xhci_state_t *state) {
    const volatile xhci_trb_t *event_ring;
    uint32_t control;
    if (state == 0 || !state->initialized) return false;
    if (state->deferred_event_count != 0U) return true;
    event_ring = (const volatile xhci_trb_t *)state->event_ring.cpu;
    if (event_ring == 0) return false;
    dma_sync_for_cpu((dma_mapping_t *)&state->event_ring.mapping);
    control = event_ring[state->event_index].control;
    return (control & XHCI_TRB_CYCLE) == state->event_cycle;
}

void xhci_event_handler_complete(xhci_state_t *state) {
    uint64_t dequeue;
    if (state == 0 || !state->initialized) return;
    dequeue = xhci_dma_address(&state->event_ring.mapping) +
              (uint64_t)state->event_index * sizeof(xhci_trb_t);
    /* EHB is RW1C: software must write one to clear the controller's event
     * handler busy state after consuming a batch.  Writing ERDP without EHB
     * leaves the bit set on QEMU and suppresses all later HID interrupts. */
    (void)xhci_controller_write64(state, state->runtime_offset + XHCI_RUNTIME_INTR0 +
                       XHCI_RUNTIME_ERDP, dequeue | XHCI_ERDP_EHB);
    (void)xhci_controller_write32(state, state->runtime_offset + XHCI_RUNTIME_INTR0,
                       XHCI_IMAN_IP | XHCI_IMAN_IE);
}



bool xhci_next_ring_event(xhci_state_t *state, xhci_trb_t *event) {
    const volatile xhci_trb_t *event_ring;
    uint32_t control;
    if (state == 0 || event == 0) return false;
    /*
     * The xHCI event ring is controller-owned.  Software consumes it solely
     * by advancing ERDP; it must never clear or otherwise rewrite an Event
     * TRB.  Clearing the cycle bit makes an empty, stale entry look valid
     * when the consumer cycle state wraps, after which HID completions are
     * permanently missed.
     */
    event_ring = (const volatile xhci_trb_t *)state->event_ring.cpu;
    dma_sync_for_cpu(&state->event_ring.mapping);
    control = event_ring[state->event_index].control;
    if ((control & XHCI_TRB_CYCLE) != state->event_cycle) {
        /* An empty Event Ring is the normal result of a coalesced wakeup. */
        return false;
    }
    event->parameter = event_ring[state->event_index].parameter;
    event->status = event_ring[state->event_index].status;
    event->control = control;
    state->event_index = (state->event_index + 1U) % XHCI_RING_TRB_COUNT;
    if (state->event_index == 0) state->event_cycle ^= 1U;
    uint64_t event_dequeue = xhci_dma_address(&state->event_ring.mapping) +
                             (uint64_t)state->event_index * sizeof(xhci_trb_t);

    /*
     * Advance ERDP for ring-space accounting, but do NOT clear EHB for every
     * individual Event TRB.
     *
     * EHB belongs to the whole event-processing batch.  Clearing it here can
     * let the controller generate another MSI-X while the current deferred
     * worker is still draining the same Event Ring.
     *
     * xhci_event_handler_complete() clears EHB exactly once after the batch.
     */
    (void)xhci_controller_write64(state, state->runtime_offset + XHCI_RUNTIME_INTR0 +
                       XHCI_RUNTIME_ERDP, event_dequeue | XHCI_ERDP_EHB);
    return true;
}

bool xhci_defer_event(xhci_state_t *state, const xhci_trb_t *event) {
    if (state == 0 || event == 0 ||
        !xhci_event_queue_push(state->deferred_events,
                               &state->deferred_event_tail,
                               &state->deferred_event_count, event)) {
        if (state != 0) xhci_set_error(28U);
        return false;
    }
    return true;
}

/* 缁熶竴浜嬩欢娑堣垂鑰呬紭鍏堝鐞嗗悓姝ョ瓑寰呴樁娈垫殏瀛樼殑浜嬩欢锛屽啀璇诲彇纭欢浜嬩欢鐜€?*/
bool xhci_next_event(xhci_state_t *state, xhci_trb_t *event) {
    if (state == 0 || event == 0) return false;
    if (xhci_event_queue_pop(state->deferred_events,
                             &state->deferred_event_head,
                             &state->deferred_event_count, event)) return true;
    return xhci_next_ring_event(state, event);
}
