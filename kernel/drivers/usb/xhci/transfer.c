#include "internal.h"

#define XHCI_TRB_CYCLE (1U << 0)
#define XHCI_TRB_CHAIN (1U << 4)
#define XHCI_TRB_INTERRUPT_ON_SHORT_PACKET (1U << 2)
#define XHCI_TRB_INTERRUPT_ON_COMPLETION (1U << 5)
#define XHCI_TRB_IMMEDIATE_DATA (1U << 6)
#define XHCI_TRB_DIRECTION_IN (1U << 16)
#define XHCI_TRB_SIA (1U << 31)
#define XHCI_TRB_TRANSFER_TYPE_SHIFT 16U
#define XHCI_TRB_TYPE_SHIFT 10U
#define XHCI_SETUP_STAGE_TYPE 2U
#define XHCI_DATA_STAGE_TYPE 3U
#define XHCI_STATUS_STAGE_TYPE 4U
#define XHCI_CONTROL_TRANSFER_IN 3U
#define XHCI_CONTROL_TRANSFER_OUT 2U

typedef struct __attribute__((packed)) xhci_transfer_trb {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
} xhci_transfer_trb_t;

_Static_assert(sizeof(xhci_transfer_trb_t) == 16U,
               "xHCI transfer TRB ABI");

static void xhci_transfer_clear(xhci_transfer_trb_t *trb) {
    trb->parameter = 0U;
    trb->status = 0U;
    trb->control = 0U;
}

static bool xhci_transfer_encode_payload(xhci_transfer_trb_t *trb,
                                          uint32_t trb_type,
                                          uint64_t dma_address,
                                          uint32_t length,
                                          uint32_t flags,
                                          uint32_t cycle) {
    uint32_t transfer_flags = XHCI_TRB_CHAIN |
                              XHCI_TRB_INTERRUPT_ON_SHORT_PACKET |
                              XHCI_TRB_INTERRUPT_ON_COMPLETION;
    if (trb == 0 || dma_address == 0U || length == 0U) return false;
    if (trb_type == 5U) {
        transfer_flags = XHCI_TRB_CHAIN |
                         XHCI_TRB_INTERRUPT_ON_COMPLETION |
                         XHCI_TRB_SIA;
    }
    xhci_transfer_clear(trb);
    trb->parameter = dma_address;
    trb->status = length;
    trb->control = (trb_type << XHCI_TRB_TYPE_SHIFT) |
                   (flags & transfer_flags) |
                   (cycle & XHCI_TRB_CYCLE);
    return true;
}

bool xhci_transfer_encode_normal(void *raw_trb, uint64_t dma_address,
                                 uint32_t length, uint32_t flags,
                                 uint32_t cycle) {
    return xhci_transfer_encode_payload((xhci_transfer_trb_t *)raw_trb, 1U,
                                         dma_address, length, flags, cycle);
}

bool xhci_transfer_encode_isoch(void *raw_trb, uint64_t dma_address,
                                uint32_t length, uint32_t flags,
                                uint32_t cycle) {
    return xhci_transfer_encode_payload((xhci_transfer_trb_t *)raw_trb, 5U,
                                         dma_address, length, flags, cycle);
}

bool xhci_transfer_encode_setup(void *raw_trb, const uint8_t setup[8],
                                uint32_t length, bool direction_in,
                                uint32_t cycle) {
    xhci_transfer_trb_t *trb = (xhci_transfer_trb_t *)raw_trb;
    uint64_t setup_value = 0U;
    if (trb == 0 || setup == 0) return false;
    for (uint32_t index = 0U; index < 8U; ++index) {
        setup_value |= (uint64_t)setup[index] << (index * 8U);
    }
    xhci_transfer_clear(trb);
    trb->parameter = setup_value;
    trb->status = 8U;
    trb->control = (XHCI_SETUP_STAGE_TYPE << XHCI_TRB_TYPE_SHIFT) |
                   XHCI_TRB_IMMEDIATE_DATA |
                   (length != 0U ?
                     ((direction_in ? XHCI_CONTROL_TRANSFER_IN :
                       XHCI_CONTROL_TRANSFER_OUT) <<
                     XHCI_TRB_TRANSFER_TYPE_SHIFT) : 0U) |
                   (cycle & XHCI_TRB_CYCLE);
    return true;
}

bool xhci_transfer_encode_data(void *raw_trb, uint64_t dma_address,
                               uint32_t length, bool direction_in,
                               uint32_t cycle) {
    xhci_transfer_trb_t *trb = (xhci_transfer_trb_t *)raw_trb;
    if (trb == 0 || dma_address == 0U || length == 0U) return false;
    xhci_transfer_clear(trb);
    trb->parameter = dma_address;
    trb->status = length;
    trb->control = (XHCI_DATA_STAGE_TYPE << XHCI_TRB_TYPE_SHIFT) |
                   (direction_in ? XHCI_TRB_DIRECTION_IN : 0U) |
                   (cycle & XHCI_TRB_CYCLE);
    return true;
}

bool xhci_transfer_encode_status(void *raw_trb, bool direction_in,
                                 uint32_t cycle) {
    xhci_transfer_trb_t *trb = (xhci_transfer_trb_t *)raw_trb;
    if (trb == 0) return false;
    xhci_transfer_clear(trb);
    trb->control = (XHCI_STATUS_STAGE_TYPE << XHCI_TRB_TYPE_SHIFT) |
                   (direction_in ? 0U : XHCI_TRB_DIRECTION_IN) |
                   XHCI_TRB_INTERRUPT_ON_COMPLETION |
                   (cycle & XHCI_TRB_CYCLE);
    return true;
}

bool xhci_transfer_encode_self_test(void) {
    xhci_transfer_trb_t setup_trb;
    xhci_transfer_trb_t data_trb;
    xhci_transfer_trb_t status_trb;
    xhci_transfer_trb_t normal_trb;
    xhci_transfer_trb_t isoch_trb;
    static const uint8_t setup[8] = {0x80U, 6U, 0U, 1U, 0U, 0U, 18U, 0U};
    if (!xhci_transfer_encode_setup(&setup_trb, setup, 18U, true, 1U) ||
        !xhci_transfer_encode_data(&data_trb, 0x12345000ULL, 18U, true, 1U) ||
        !xhci_transfer_encode_status(&status_trb, true, 1U) ||
        !xhci_transfer_encode_normal(&normal_trb, 0x12346000ULL, 32U,
                                     XHCI_TRB_INTERRUPT_ON_COMPLETION |
                                     XHCI_TRB_DIRECTION_IN, 1U) ||
        !xhci_transfer_encode_isoch(&isoch_trb, 0x12347000ULL, 48U,
                                    XHCI_TRB_INTERRUPT_ON_COMPLETION |
                                    XHCI_TRB_DIRECTION_IN | XHCI_TRB_SIA, 1U)) {
        return false;
    }
    return setup_trb.parameter == 0x0012000001000680ULL &&
           setup_trb.status == 8U &&
           (setup_trb.control & (0x3FU << XHCI_TRB_TYPE_SHIFT)) ==
               (XHCI_SETUP_STAGE_TYPE << XHCI_TRB_TYPE_SHIFT) &&
           data_trb.parameter == 0x12345000ULL && data_trb.status == 18U &&
           (data_trb.control & XHCI_TRB_DIRECTION_IN) != 0U &&
           (status_trb.control & XHCI_TRB_INTERRUPT_ON_COMPLETION) != 0U &&
           normal_trb.parameter == 0x12346000ULL && normal_trb.status == 32U &&
           (normal_trb.control & (0x3FU << XHCI_TRB_TYPE_SHIFT)) ==
               (1U << XHCI_TRB_TYPE_SHIFT) &&
           (normal_trb.control & XHCI_TRB_DIRECTION_IN) == 0U &&
           isoch_trb.parameter == 0x12347000ULL && isoch_trb.status == 48U &&
           (isoch_trb.control & (0x3FU << XHCI_TRB_TYPE_SHIFT)) ==
               (5U << XHCI_TRB_TYPE_SHIFT) &&
           (isoch_trb.control & XHCI_TRB_DIRECTION_IN) == 0U &&
           (isoch_trb.control & XHCI_TRB_SIA) != 0U;
}
