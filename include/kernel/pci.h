#pragma once

#include <arch/x86_64/acpi.h>
#include <kernel/device.h>

#define PCI_MAX_DEVICES 512U
#define PCI_MAX_BARS    6U

enum pci_resource_flags {
    PCI_RESOURCE_IO         = 1U << 0,
    PCI_RESOURCE_MEMORY     = 1U << 1,
    PCI_RESOURCE_PREFETCH   = 1U << 2,
    PCI_RESOURCE_64BIT      = 1U << 3,
};

typedef struct pci_bar {
    uint64_t address;
    uint64_t length;
    uint32_t flags;
    uint32_t reserved;
} pci_bar_t;

typedef struct pci_device {
    device_t device;
    uint16_t segment;
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint8_t ecam_start_bus;
    uint8_t ecam_end_bus;
    uint8_t revision;
    uint8_t prog_if;
    uint8_t subclass;
    uint8_t class_code;
    uint8_t header_type;
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t command;
    uint16_t status;
    pci_bar_t bars[PCI_MAX_BARS];
    resource_t resources[PCI_MAX_BARS];
    uint8_t msix_capability;
    uint8_t msi_capability;
    uint8_t reserved0[2];
    uint16_t msix_table_size;
    uint8_t msix_table_bar;
    uint8_t reserved1[5];
    uint32_t msix_table_offset;
} pci_device_t;

typedef struct pci_host {
    bool initialized;
    uint32_t ecam_count;
    uint32_t device_count;
    x86_acpi_ecam_t ecam[X86_MAX_ECAM_SEGMENTS];
    pci_device_t devices[PCI_MAX_DEVICES];
} pci_host_t;

kstatus_t pci_ecam_init(pci_host_t *host);
const pci_device_t *pci_find_device(const pci_host_t *host, uint16_t vendor_id,
                                    uint16_t device_id);
const pci_device_t *pci_find_class(const pci_host_t *host, uint8_t class_code,
                                   uint8_t subclass, uint8_t prog_if);
kstatus_t pci_msix_table(const pci_device_t *device, paddr_t *physical,
                         uint16_t *entry_count);
kstatus_t pci_msix_configure(pci_device_t *device, uint16_t entry,
                             uint32_t apic_id, uint8_t vector);
kstatus_t pci_msix_mask(pci_device_t *device, uint16_t entry, bool masked);
kstatus_t pci_enable_memory_busmaster(pci_device_t *device);
kstatus_t pci_msi_configure(pci_device_t *device, uint32_t apic_id,
                            uint8_t vector);
const pci_host_t *pci_current_host(void);
bool pci_ecam_self_test(void);
uint32_t pci_ecam_last_error(void);
