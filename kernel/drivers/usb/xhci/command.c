#include "internal.h"

#define XHCI_TRB_TYPE_SHIFT 10U
#define XHCI_TRB_SLOT_SHIFT 24U
#define XHCI_TRB_ENDPOINT_SHIFT 16U

typedef struct __attribute__((packed)) xhci_command_trb {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
} xhci_command_trb_t;

_Static_assert(sizeof(xhci_command_trb_t) == 16U, "xHCI command TRB ABI");

void xhci_command_encode(void *raw_trb, uint32_t type, uint8_t slot,
                         uint8_t endpoint, uint64_t parameter,
                         uint32_t cycle) {
    xhci_command_trb_t *trb = (xhci_command_trb_t *)raw_trb;
    if (trb == 0) return;
    trb->parameter = parameter;
    trb->status = 0U;
    trb->control = (type << XHCI_TRB_TYPE_SHIFT) |
                   ((uint32_t)slot << XHCI_TRB_SLOT_SHIFT) |
                   ((uint32_t)(endpoint & 0x1FU) << XHCI_TRB_ENDPOINT_SHIFT) |
                   cycle;
}
