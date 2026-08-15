#ifndef LITEOS_PCI_H
#define LITEOS_PCI_H

#include "uefi.h"

#define LITEOS_PCI_MAX_DEVICES 256U

typedef struct {
    UINT8 Bus;
    UINT8 Device;
    UINT8 Function;
    UINT8 HeaderType;
    UINT16 VendorId;
    UINT16 DeviceId;
    UINT8 RevisionId;
    UINT8 ProgIf;
    UINT8 Subclass;
    UINT8 ClassCode;
    UINT8 InterruptLine;
    UINT8 InterruptPin;
    UINT16 Reserved;
    UINT64 Bars[6];
} LITEOS_PCI_DEVICE;

typedef struct {
    LITEOS_PCI_DEVICE Devices[LITEOS_PCI_MAX_DEVICES];
    UINT32 DeviceCount;
    BOOLEAN Initialized;
} LITEOS_PCI_BUS;

BOOLEAN liteos_pci_init(LITEOS_PCI_BUS *bus);
const LITEOS_PCI_DEVICE *liteos_pci_find_class(const LITEOS_PCI_BUS *bus,
                                               UINT8 class_code, UINT8 subclass,
                                               UINT8 prog_if);

#endif
