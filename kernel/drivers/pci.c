#include "pci.h"

#define PCI_CONFIG_ADDRESS 0x0CF8U
#define PCI_CONFIG_DATA    0x0CFCU

static VOID io_out32(UINT16 port, UINT32 value) {
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static UINT32 io_in32(UINT16 port) {
    UINT32 value;
    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static UINT32 pci_read_config32(UINT8 bus, UINT8 device, UINT8 function, UINT8 offset) {
    UINT32 address = 0x80000000U | ((UINT32)bus << 16) |
                     ((UINT32)device << 11) | ((UINT32)function << 8) |
                     ((UINT32)offset & 0xFCU);
    io_out32(PCI_CONFIG_ADDRESS, address);
    return io_in32(PCI_CONFIG_DATA);
}

static VOID clear_device(LITEOS_PCI_DEVICE *device) {
    UINT8 *bytes = (UINT8 *)device;
    for (UINTN i = 0; i < sizeof(*device); ++i) bytes[i] = 0;
}

static VOID read_bars(LITEOS_PCI_DEVICE *device) {
    if ((device->HeaderType & 0x7FU) != 0) return;
    for (UINT32 index = 0; index < 6U; ++index) {
        UINT32 low = pci_read_config32(device->Bus, device->Device,
                                       device->Function, (UINT8)(0x10U + index * 4U));
        if (low == 0) continue;
        if ((low & 1U) != 0) {
            device->Bars[index] = (UINT64)(low & ~3U);
            continue;
        }
        UINT32 memory_type = (low >> 1) & 3U;
        device->Bars[index] = (UINT64)(low & ~0xFU);
        if (memory_type == 2U && index + 1U < 6U) {
            UINT32 high = pci_read_config32(device->Bus, device->Device,
                                            device->Function,
                                            (UINT8)(0x10U + (index + 1U) * 4U));
            device->Bars[index] |= (UINT64)high << 32;
            ++index;
        }
    }
}

BOOLEAN liteos_pci_init(LITEOS_PCI_BUS *bus) {
    if (bus == 0 || bus->Initialized) return 0;
    bus->DeviceCount = 0;
    for (UINT32 i = 0; i < LITEOS_PCI_MAX_DEVICES; ++i) clear_device(&bus->Devices[i]);

    for (UINT32 bus_number = 0; bus_number < 256U; ++bus_number) {
        for (UINT32 device_number = 0; device_number < 32U; ++device_number) {
            for (UINT32 function_number = 0; function_number < 8U; ++function_number) {
                UINT32 id = pci_read_config32((UINT8)bus_number, (UINT8)device_number,
                                              (UINT8)function_number, 0);
                if ((id & 0xFFFFU) == 0xFFFFU) continue;
                if (bus->DeviceCount >= LITEOS_PCI_MAX_DEVICES) {
                    bus->Initialized = 0;
                    return 0;
                }
                LITEOS_PCI_DEVICE *device = &bus->Devices[bus->DeviceCount++];
                UINT32 class_data = pci_read_config32((UINT8)bus_number,
                                                      (UINT8)device_number,
                                                      (UINT8)function_number, 8);
                UINT32 header_data = pci_read_config32((UINT8)bus_number,
                                                       (UINT8)device_number,
                                                       (UINT8)function_number, 12);
                UINT32 interrupt_data = pci_read_config32((UINT8)bus_number,
                                                           (UINT8)device_number,
                                                           (UINT8)function_number, 0x3C);
                device->Bus = (UINT8)bus_number;
                device->Device = (UINT8)device_number;
                device->Function = (UINT8)function_number;
                device->VendorId = (UINT16)(id & 0xFFFFU);
                device->DeviceId = (UINT16)(id >> 16);
                device->RevisionId = (UINT8)(class_data & 0xFFU);
                device->ProgIf = (UINT8)((class_data >> 8) & 0xFFU);
                device->Subclass = (UINT8)((class_data >> 16) & 0xFFU);
                device->ClassCode = (UINT8)(class_data >> 24);
                device->HeaderType = (UINT8)((header_data >> 16) & 0xFFU);
                device->InterruptLine = (UINT8)(interrupt_data & 0xFFU);
                device->InterruptPin = (UINT8)((interrupt_data >> 8) & 0xFFU);
                read_bars(device);
            }
        }
    }
    bus->Initialized = 1;
    return 1;
}

const LITEOS_PCI_DEVICE *liteos_pci_find_class(const LITEOS_PCI_BUS *bus,
                                               UINT8 class_code, UINT8 subclass,
                                               UINT8 prog_if) {
    if (bus == 0 || !bus->Initialized) return 0;
    for (UINT32 i = 0; i < bus->DeviceCount; ++i) {
        const LITEOS_PCI_DEVICE *device = &bus->Devices[i];
        if (device->ClassCode == class_code && device->Subclass == subclass &&
            (prog_if == 0xFFU || device->ProgIf == prog_if)) return device;
    }
    return 0;
}
