#include <kernel/pci.h>
#include "internal.h"

#define XHCI_CLASS_SERIAL_BUS 0x0CU
#define XHCI_SUBCLASS_USB     0x03U
#define XHCI_PROG_IF          0x30U

const struct pci_device *xhci_pci_find_controller(void) {
    const pci_host_t *host = pci_current_host();
    if (host == 0) return 0;
    return pci_find_class(host,
                          XHCI_CLASS_SERIAL_BUS,
                          XHCI_SUBCLASS_USB,
                          XHCI_PROG_IF);
}

uint32_t xhci_pci_controller_count(void) {
    const pci_host_t *host = pci_current_host();
    uint32_t count = 0U;
    if (host == 0) return 0U;
    for (uint32_t index = 0U; index < host->device_count; ++index) {
        const pci_device_t *device = &host->devices[index];
        if (device->class_code == XHCI_CLASS_SERIAL_BUS &&
            device->subclass == XHCI_SUBCLASS_USB &&
            device->prog_if == XHCI_PROG_IF) {
            ++count;
        }
    }
    return count;
}

const struct pci_device *xhci_pci_controller_at(uint32_t wanted) {
    const pci_host_t *host = pci_current_host();
    uint32_t count = 0U;
    if (host == 0) return 0;
    for (uint32_t index = 0U; index < host->device_count; ++index) {
        const pci_device_t *device = &host->devices[index];
        if (device->class_code != XHCI_CLASS_SERIAL_BUS ||
            device->subclass != XHCI_SUBCLASS_USB ||
            device->prog_if != XHCI_PROG_IF) continue;
        if (count++ == wanted) return device;
    }
    return 0;
}

