#pragma once

#include "base.h"
#include "mm.h"

struct device;

enum iommu_map_access {
    IOMMU_MAP_DEVICE_READ  = 1U << 0,
    IOMMU_MAP_DEVICE_WRITE = 1U << 1,
};

bool iommu_init(void);
bool iommu_available(void);
bool iommu_hardware_enabled(void);
kstatus_t iommu_attach_pci_device(struct device *device, uint16_t segment,
                                  uint8_t bus, uint8_t slot, uint8_t function);
kstatus_t iommu_detach_device(struct device *device);
kstatus_t iommu_map_pages(struct device *device, iova_t iova, page_t **pages,
                          uint32_t page_count, uint32_t access);
kstatus_t iommu_unmap_pages(struct device *device, iova_t iova, uint64_t length);
bool iommu_self_test(void);
