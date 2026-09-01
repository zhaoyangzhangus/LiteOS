#include "internal.h"

void xhci_device_context_clear(void *context, uint64_t size) {
    if (context == 0) return;
    uint8_t *bytes = (uint8_t *)context;
    for (uint64_t index = 0; index < size; ++index) bytes[index] = 0U;
}

bool xhci_device_context_self_test(void) {
    uint8_t context[17];
    for (uint64_t index = 0; index < sizeof(context); ++index) {
        context[index] = 0xA5U;
    }
    xhci_device_context_clear(context, sizeof(context));
    for (uint64_t index = 0; index < sizeof(context); ++index) {
        if (context[index] != 0U) return false;
    }
    return true;
}
