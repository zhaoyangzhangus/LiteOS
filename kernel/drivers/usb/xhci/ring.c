#include "internal.h"

#define XHCI_LINK_TRB_TYPE 6U
#define XHCI_TRB_CYCLE (1U << 0)
#define XHCI_TRB_LINK_TOGGLE_CYCLE (1U << 1)
#define XHCI_TRB_TYPE_SHIFT 10U

typedef struct __attribute__((packed)) xhci_ring_trb {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
} xhci_ring_trb_t;

_Static_assert(sizeof(xhci_ring_trb_t) == 16U, "xHCI TRB ABI");

void xhci_ring_init_link(void *cpu, uint64_t dma_address,
                         uint32_t trb_count) {
    if (cpu == 0 || trb_count == 0U) return;
    xhci_ring_trb_t *trbs = (xhci_ring_trb_t *)cpu;
    xhci_ring_trb_t *link = &trbs[trb_count - 1U];
    link->parameter = dma_address;
    link->control = (XHCI_LINK_TRB_TYPE << XHCI_TRB_TYPE_SHIFT) |
                    XHCI_TRB_CYCLE | XHCI_TRB_LINK_TOGGLE_CYCLE;
}
