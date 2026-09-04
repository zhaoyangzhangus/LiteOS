#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <kernel/base.h>
#include <kernel/pci.h>

/*
 * The vendor boundary deliberately exposes only opaque handles and facts
 * obtained from the vendor RM.  nv_state_t, OBJGPU, and the generated NVOC
 * types stay on the vendor side of this interface.
 */
typedef struct nv_rm_vendor_result {
    void *context;
    void *rm_gpu;
    void *rm_device;
    uint32_t chip_arch;
    uint32_t chip_impl;
    uint32_t hal_impl;
    bool blackwell;
    bool gb205;
    bool hal_valid;
} nv_rm_vendor_result_t;

kstatus_t nv_rm_vendor_start(const pci_device_t *device,
                             void **context_out,
                             nv_rm_vendor_result_t *result);

void nv_rm_vendor_stop(void *context);
