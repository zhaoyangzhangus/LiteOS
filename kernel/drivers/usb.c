#include "usb.h"

#define USB_SERIAL_CLASS 0x0CU
#define USB_SUBCLASS     0x03U
#define USB_XHCI_PROGRAMMING 0x30U
#define USB_TRB_TYPE_SHIFT 10U
#define USB_TRB_TYPE_NORMAL 1U
#define USB_TRB_INTERRUPT_ON_COMPLETION (1U << 5)
#define USB_TRB_CHAIN_BIT (1U << 4)

static VOID memory_zero(UINT8 *memory, UINT64 size) {
    while (size-- != 0) *memory++ = 0;
}

BOOLEAN liteos_usb_is_xhci(const LITEOS_PCI_DEVICE *device) {
    return device != 0 && device->ClassCode == USB_SERIAL_CLASS &&
           device->Subclass == USB_SUBCLASS && device->ProgIf == USB_XHCI_PROGRAMMING;
}

BOOLEAN liteos_xhci_init(LITEOS_XHCI_CONTROLLER *controller,
                         const LITEOS_PCI_DEVICE *device) {
    if (controller == 0 || controller->Initialized || !liteos_usb_is_xhci(device) ||
        device->Bars[0] == 0) return 0;
    controller->PciDevice = *device;
    if (!liteos_buddy_alloc(LITEOS_BUDDY_MIN_ORDER, &controller->TransferRingBlock)) return 0;
    controller->TransferRing = (LITEOS_USB_TRB *)
        (uintptr_t)controller->TransferRingBlock.PhysicalAddress;
    memory_zero((UINT8 *)controller->TransferRing, LITEOS_BUDDY_MIN_BLOCK_SIZE);
    controller->EnqueueIndex = 0;
    controller->DequeueIndex = 0;
    controller->CycleState = 1;
    controller->Initialized = 1;
    return 1;
}

BOOLEAN liteos_xhci_destroy(LITEOS_XHCI_CONTROLLER *controller) {
    if (controller == 0 || !controller->Initialized) return 0;
    if (!liteos_buddy_free(&controller->TransferRingBlock)) return 0;
    controller->TransferRing = 0;
    controller->Initialized = 0;
    return 1;
}

BOOLEAN liteos_xhci_submit_transfer(LITEOS_XHCI_CONTROLLER *controller,
                                    UINT64 physical_buffer, UINT32 length,
                                    UINT32 transfer_type) {
    UINT16 next_index;
    if (controller == 0 || !controller->Initialized || physical_buffer == 0 ||
        (physical_buffer & 0xFULL) != 0 || length == 0) return 0;
    next_index = (UINT16)((controller->EnqueueIndex + 1U) %
                          LITEOS_USB_TRANSFER_RING_SIZE);
    if (next_index == controller->DequeueIndex) return 0;
    LITEOS_USB_TRB *trb = &controller->TransferRing[controller->EnqueueIndex];
    trb->ParameterLow = (UINT32)physical_buffer;
    trb->ParameterHigh = (UINT32)(physical_buffer >> 32);
    trb->Status = length & 0x1FFFFU;
    trb->Control = ((transfer_type & 31U) << USB_TRB_TYPE_SHIFT) |
                   USB_TRB_CHAIN_BIT | USB_TRB_INTERRUPT_ON_COMPLETION |
                   (controller->CycleState & 1U);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    controller->EnqueueIndex = next_index;
    return 1;
}
