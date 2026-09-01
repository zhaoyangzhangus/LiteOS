#include <kernel/pci.h>
#include "internal.h"

#define E1000_VENDOR_ID 0x8086U

const struct pci_device *e1000_pci_find(void) {
    const pci_host_t *host = pci_current_host();
    const pci_device_t *pci = host == 0 ? 0 :
        pci_find_class(host, 0x02U, 0x00U, 0xFFU);
    return pci != 0 && pci->vendor_id == E1000_VENDOR_ID ? pci : 0;
}
