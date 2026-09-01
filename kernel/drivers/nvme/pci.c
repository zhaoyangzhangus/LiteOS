#include "internal.h"

#define NVME_CLASS_MASS_STORAGE 0x01U
#define NVME_SUBCLASS_NVM        0x08U
#define NVME_PROGIF_NVM          0x02U

bool nvme_pci_is_controller(const pci_device_t *pci) {
    return pci != 0 &&
           pci->class_code == NVME_CLASS_MASS_STORAGE &&
           pci->subclass == NVME_SUBCLASS_NVM &&
           pci->prog_if == NVME_PROGIF_NVM;
}
