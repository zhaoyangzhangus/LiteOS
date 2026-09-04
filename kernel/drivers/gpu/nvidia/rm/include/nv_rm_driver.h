#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <kernel/base.h>
#include <kernel/pci.h>

#include "../../port/include/nv_port.h"

typedef enum nv_gpu_state {
    NV_GPU_NEW = 0,
    NV_GPU_PCI_READY,
    NV_GPU_BAR_READY,
    NV_GPU_IRQ_READY,
    NV_GPU_RM_READY,
    NV_GPU_HAL_READY,
    NV_GPU_GSP_READY,
    NV_GPU_FAILED,
} nv_gpu_state_t;

typedef struct nv_gpu_device {
    nv_gpu_state_t state;
    pci_device_t *pci;

    uint8_t bar0_index;
    uint8_t bar1_index;
    uint64_t bar0_pa;
    uint64_t bar0_size;
    void *bar0_va;
    uint64_t bar1_pa;
    uint64_t bar1_size;

    nv_port_mmio_mapping_t bar0_mapping;
    void *irq_set;
    void *rm_gpu;
    void *rm_device;
    void *vendor_context;

    uint16_t segment;
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint16_t vendor_id;
    uint16_t device_id;

    uint32_t failure_stage;
    kstatus_t failure_status;
} nv_gpu_device_t;

kstatus_t nv_rm_driver_probe(void);

const nv_gpu_device_t *nv_rm_driver_current(void);
