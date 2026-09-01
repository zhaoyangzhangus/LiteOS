#include "internal.h"

bool xhci_event_queue_push(xhci_trb_t *entries, uint32_t *tail,
                           uint32_t *count, const xhci_trb_t *event) {
    if (entries == 0 || tail == 0 || count == 0 || event == 0 ||
        *count >= XHCI_DEFERRED_EVENT_COUNT ||
        *tail >= XHCI_DEFERRED_EVENT_COUNT) {
        return false;
    }
    entries[*tail] = *event;
    *tail = (*tail + 1U) % XHCI_DEFERRED_EVENT_COUNT;
    ++*count;
    return true;
}

bool xhci_event_queue_pop(const xhci_trb_t *entries, uint32_t *head,
                          uint32_t *count, xhci_trb_t *event) {
    if (entries == 0 || head == 0 || count == 0 || event == 0 ||
        *count == 0U || *head >= XHCI_DEFERRED_EVENT_COUNT) {
        return false;
    }
    *event = entries[*head];
    *head = (*head + 1U) % XHCI_DEFERRED_EVENT_COUNT;
    --*count;
    return true;
}

bool xhci_event_queue_self_test(void) {
    xhci_trb_t entries[XHCI_DEFERRED_EVENT_COUNT];
    xhci_trb_t expected;
    xhci_trb_t actual;
    uint32_t head = 0U;
    uint32_t tail = 0U;
    uint32_t count = 0U;

    for (uint32_t index = 0U; index < XHCI_DEFERRED_EVENT_COUNT; ++index) {
        expected.parameter = 0x1000U + index;
        expected.status = 0x2000U + index;
        expected.control = 0x3000U + index;
        if (!xhci_event_queue_push(entries, &tail, &count, &expected)) {
            return false;
        }
    }
    expected.parameter = 0xFFFFU;
    expected.status = 0xEEEEU;
    expected.control = 0xDDDDU;
    if (xhci_event_queue_push(entries, &tail, &count, &expected)) {
        return false;
    }
    for (uint32_t index = 0U; index < XHCI_DEFERRED_EVENT_COUNT; ++index) {
        if (!xhci_event_queue_pop(entries, &head, &count, &actual) ||
            actual.parameter != 0x1000U + index ||
            actual.status != 0x2000U + index ||
            actual.control != 0x3000U + index) {
            return false;
        }
    }
    return count == 0U && !xhci_event_queue_pop(entries, &head, &count,
                                                  &actual);
}
