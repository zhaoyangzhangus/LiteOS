#ifndef LITEOS_USB_H
#define LITEOS_USB_H

#include "buddy.h"
#include "pci.h"

#define LITEOS_USB_TRANSFER_RING_SIZE 16U

typedef struct __attribute__((packed)) {
    UINT32 ParameterLow;
    UINT32 ParameterHigh;
    UINT32 Status;
    UINT32 Control;
} LITEOS_USB_TRB;

typedef struct {
    LITEOS_PCI_DEVICE PciDevice;
    LITEOS_PHYSICAL_BLOCK TransferRingBlock;
    LITEOS_USB_TRB *TransferRing;
    UINT16 EnqueueIndex;
    UINT16 DequeueIndex;
    UINT8 CycleState;
    BOOLEAN Initialized;
} LITEOS_XHCI_CONTROLLER;

BOOLEAN liteos_usb_is_xhci(const LITEOS_PCI_DEVICE *device);
BOOLEAN liteos_xhci_init(LITEOS_XHCI_CONTROLLER *controller,
                         const LITEOS_PCI_DEVICE *device);
BOOLEAN liteos_xhci_destroy(LITEOS_XHCI_CONTROLLER *controller);
BOOLEAN liteos_xhci_submit_transfer(LITEOS_XHCI_CONTROLLER *controller,
                                    UINT64 physical_buffer, UINT32 length,
                                    UINT32 transfer_type);

#endif
