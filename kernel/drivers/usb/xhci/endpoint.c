#include "internal.h"

void xhci_endpoint_context_encode(uint32_t endpoint[5], uint8_t interval,
                                  uint8_t type, uint16_t max_packet,
                                  uint64_t ring_address) {
    if (endpoint == 0) return;
    endpoint[0] = (uint32_t)interval << 16;
    endpoint[1] = (3U << 1) | ((uint32_t)type << 3) |
                  ((uint32_t)max_packet << 16);
    endpoint[2] = (uint32_t)ring_address | 1U;
    endpoint[3] = (uint32_t)(ring_address >> 32);
    endpoint[4] = 0U;
}

void xhci_endpoint_context_set_max_burst(uint32_t endpoint[5],
                                         uint8_t max_burst) {
    if (endpoint == 0) return;
    endpoint[1] &= ~(0xFFU << 8);
    endpoint[1] |= (uint32_t)max_burst << 8;
}

void xhci_endpoint_context_set_average_trb_length(uint32_t endpoint[5],
                                                  uint16_t length) {
    if (endpoint == 0 || length == 0U) return;
    endpoint[4] &= 0xFFFF0000U;
    endpoint[4] |= length;
}

void xhci_endpoint_context_set_max_esit_payload(uint32_t endpoint[5],
                                                uint32_t length) {
    if (endpoint == 0 || length == 0U) return;
    endpoint[0] &= 0x00FFFFFFU;
    endpoint[0] |= ((length >> 16) & 0xFFU) << 24;
    endpoint[4] &= 0x0000FFFFU;
    endpoint[4] |= (length & 0xFFFFU) << 16;
}

void xhci_endpoint_context_set_streams(uint32_t endpoint[5],
                                       uint64_t stream_array,
                                       uint8_t max_primary_streams) {
    if (endpoint == 0 || stream_array == 0U) return;
    endpoint[0] &= ~((0x1FU << 10) | (1U << 15));
    endpoint[0] |= ((uint32_t)max_primary_streams << 10) | (1U << 15);
    endpoint[2] = (uint32_t)stream_array;
    endpoint[3] = (uint32_t)(stream_array >> 32);
}

bool xhci_endpoint_context_self_test(void) {
    uint32_t endpoint[5] = {0U, 0U, 0U, 0U, 0U};
    const uint64_t ring_address = 0x1122334455667788ULL;
    xhci_endpoint_context_encode(endpoint, 9U, 7U, 64U, ring_address);
    xhci_endpoint_context_set_average_trb_length(endpoint, 64U);
    xhci_endpoint_context_set_max_esit_payload(endpoint, 64U);
    return endpoint[0] == (9U << 16) &&
           endpoint[1] == ((3U << 1) | (7U << 3) | (64U << 16)) &&
           endpoint[2] == 0x55667789U &&
           endpoint[3] == 0x11223344U &&
           endpoint[4] == ((64U << 16) | 64U);
}
