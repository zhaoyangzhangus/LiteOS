#include "nv_rm_driver.h"

#include <kernel/console.h>

#include "nv_rm_vendor_bridge.h"

#define NVIDIA_VENDOR_ID 0x10deU
#define NVIDIA_GB205_DEVICE_ID 0x2f04U

#ifndef NULL
#define NULL ((void *)0)
#endif

enum nv_rm_failure_stage {
    NV_RM_FAILURE_NONE = 0,
    NV_RM_FAILURE_PCI,
    NV_RM_FAILURE_BAR,
    NV_RM_FAILURE_RM,
    NV_RM_FAILURE_HAL,
};

static nv_gpu_device_t g_nv_gpu;

static void nv_rm_log_status(const char *prefix, kstatus_t status)
{
    liteos_serial_printf_serial_only("%s status=%d\r\n", prefix, (int)status);
}

/*
 * The production LiteOS image does not link the vendor RM object.  Keep that
 * image honest and buildable: the unresolved vendor boundary is an explicit
 * failure, never a successful no-op.  The Phase 4 image supplies the strong
 * implementation from the immutable 595.84 object set.
 */
__attribute__((weak)) kstatus_t nv_rm_vendor_start(const pci_device_t *device,
                                                   void **context_out,
                                                   nv_rm_vendor_result_t *result)
{
    (void)device;
    if (context_out != NULL)
        *context_out = NULL;
    if (result != NULL)
        *result = (nv_rm_vendor_result_t){0};

    liteos_serial_write_serial_only("LITEOS_NVIDIA_RM_NOT_LINKED\r\n");
    return K_ENOSYS;
}

__attribute__((weak)) void nv_rm_vendor_stop(void *context)
{
    (void)context;
}

static void nv_rm_driver_reset(void)
{
    g_nv_gpu = (nv_gpu_device_t){0};
    g_nv_gpu.state = NV_GPU_NEW;
}

static void nv_rm_driver_fail(uint32_t stage, kstatus_t status)
{
    g_nv_gpu.failure_stage = stage;
    g_nv_gpu.failure_status = status;
    g_nv_gpu.state = NV_GPU_FAILED;
    nv_rm_log_status("LITEOS_NVIDIA_INIT_FAILED", status);
}

static void nv_rm_driver_unwind(void)
{
    if (g_nv_gpu.vendor_context != NULL) {
        nv_rm_vendor_stop(g_nv_gpu.vendor_context);
        g_nv_gpu.vendor_context = NULL;
        g_nv_gpu.rm_gpu = NULL;
        g_nv_gpu.rm_device = NULL;
    }

    if (g_nv_gpu.bar0_mapping.active) {
        nv_port_mmio_unmap(&g_nv_gpu.bar0_mapping);
        g_nv_gpu.bar0_va = NULL;
    }
}

static bool nv_rm_memory_bar(const pci_bar_t *bar)
{
    return bar != NULL &&
           (bar->flags & PCI_RESOURCE_MEMORY) != 0U &&
           (bar->length != 0U) &&
           (bar->address != 0U);
}

static kstatus_t nv_rm_find_memory_bars(pci_device_t *device)
{
    uint32_t memory_bar = 0U;
    uint32_t i;

    for (i = 0U; i < PCI_MAX_BARS; ++i) {
        const pci_bar_t *bar = &device->bars[i];
        if (bar->length != 0U || bar->address != 0U || bar->flags != 0U) {
            liteos_serial_printf_serial_only(
                "LITEOS_NVIDIA_BAR index=%u flags=%x pa=%llx size=%llx\r\n",
                (unsigned)i, (unsigned)bar->flags,
                (unsigned long long)bar->address,
                (unsigned long long)bar->length);
        }
        if (!nv_rm_memory_bar(&device->bars[i]))
            continue;

        if (memory_bar == 0U) {
            g_nv_gpu.bar0_index = (uint8_t)i;
            g_nv_gpu.bar0_pa = device->bars[i].address;
            g_nv_gpu.bar0_size = device->bars[i].length;
        } else if (memory_bar == 1U) {
            g_nv_gpu.bar1_index = (uint8_t)i;
            g_nv_gpu.bar1_pa = device->bars[i].address;
            g_nv_gpu.bar1_size = device->bars[i].length;
        }

        ++memory_bar;
    }

    if (memory_bar < 2U || g_nv_gpu.bar0_size == 0U ||
        g_nv_gpu.bar1_size == 0U)
        return K_EIO;

    return K_OK;
}

kstatus_t nv_rm_driver_probe(void)
{
    const pci_host_t *host = pci_current_host();
    const pci_device_t *found;
    nv_rm_vendor_result_t result = {0};
    kstatus_t status;

    if (g_nv_gpu.state == NV_GPU_HAL_READY)
        return K_OK;

    if (g_nv_gpu.state == NV_GPU_FAILED)
        nv_rm_driver_reset();

    if (host == NULL || !host->initialized) {
        liteos_serial_write_serial_only("LITEOS_NVIDIA_PCI_UNAVAILABLE\r\n");
        return K_OK;
    }

    found = nv_port_pci_find(host, NVIDIA_VENDOR_ID,
                             NVIDIA_GB205_DEVICE_ID);
    if (found == NULL) {
        liteos_serial_write_serial_only("LITEOS_NVIDIA_PCI_ABSENT\r\n");
        return K_OK;
    }

    g_nv_gpu.pci = (pci_device_t *)(void *)found;
    g_nv_gpu.segment = found->segment;
    g_nv_gpu.bus = found->bus;
    g_nv_gpu.slot = found->slot;
    g_nv_gpu.function = found->function;
    g_nv_gpu.vendor_id = found->vendor_id;
    g_nv_gpu.device_id = found->device_id;

    liteos_serial_printf_serial_only(
        "LITEOS_NVIDIA_PCI_FOUND %x:%x:%x.%u ID=%x:%x\r\n",
        (unsigned)g_nv_gpu.segment, (unsigned)g_nv_gpu.bus,
        (unsigned)g_nv_gpu.slot, (unsigned)g_nv_gpu.function,
        (unsigned)g_nv_gpu.vendor_id, (unsigned)g_nv_gpu.device_id);

    status = nv_rm_find_memory_bars(g_nv_gpu.pci);
    if (status != K_OK) {
        nv_rm_driver_fail(NV_RM_FAILURE_BAR, status);
        return status;
    }

    status = nv_port_pci_enable(g_nv_gpu.pci);
    if (status != K_OK) {
        nv_rm_driver_fail(NV_RM_FAILURE_PCI, status);
        return status;
    }
    g_nv_gpu.state = NV_GPU_PCI_READY;

    liteos_serial_printf_serial_only(
        "LITEOS_NVIDIA_BAR1_METADATA index=%u pa=%llx size=%llx\r\n",
        (unsigned)g_nv_gpu.bar1_index,
        (unsigned long long)g_nv_gpu.bar1_pa,
        (unsigned long long)g_nv_gpu.bar1_size);

    liteos_serial_printf_serial_only(
        "LITEOS_NVIDIA_BAR0_REQUEST index=%u pa=%llx size=%llx\r\n",
        (unsigned)g_nv_gpu.bar0_index,
        (unsigned long long)g_nv_gpu.bar0_pa,
        (unsigned long long)g_nv_gpu.bar0_size);

    status = nv_port_mmio_map_bar(g_nv_gpu.pci, g_nv_gpu.bar0_index,
                                  NV_PORT_CACHE_UC, &g_nv_gpu.bar0_mapping);
    if (status != K_OK) {
        nv_rm_log_status("LITEOS_NVIDIA_BAR0_MAP_FAILED", status);
        nv_rm_driver_fail(NV_RM_FAILURE_BAR, status);
        return status;
    }
    g_nv_gpu.bar0_va = g_nv_gpu.bar0_mapping.base;
    g_nv_gpu.state = NV_GPU_BAR_READY;

    liteos_serial_printf_serial_only(
        "LITEOS_NVIDIA_BAR0_READY index=%u pa=%llx va=%p size=%llx\r\n",
        (unsigned)g_nv_gpu.bar0_index,
        (unsigned long long)g_nv_gpu.bar0_pa, g_nv_gpu.bar0_va,
        (unsigned long long)g_nv_gpu.bar0_size);

    status = nv_rm_vendor_start(g_nv_gpu.pci, &g_nv_gpu.vendor_context,
                                &result);
    if (status != K_OK) {
        nv_rm_driver_unwind();
        nv_rm_driver_fail(NV_RM_FAILURE_RM, status);
        return status;
    }

    /* Phase 4 stops at the vendor OBJGPU/NVOC/HAL discovery object.  The
     * internal device handle is created by the later adapter/GSP path and is
     * intentionally not required at this phase boundary. */
    if (g_nv_gpu.vendor_context == NULL || result.rm_gpu == NULL) {
        nv_rm_driver_unwind();
        nv_rm_driver_fail(NV_RM_FAILURE_RM, K_EIO);
        return K_EIO;
    }
    g_nv_gpu.rm_gpu = result.rm_gpu;
    g_nv_gpu.rm_device = result.rm_device;
    g_nv_gpu.state = NV_GPU_RM_READY;
    liteos_serial_write_serial_only("LITEOS_NVIDIA_PHASE4_RM_READY\r\n");

    liteos_serial_printf_serial_only(
        "LITEOS_NVIDIA_ARCH=%x IMPL=%x HAL=%x\r\n",
        (unsigned)result.chip_arch, (unsigned)result.chip_impl,
        (unsigned)result.hal_impl);

    if (!result.blackwell || !result.gb205 || !result.hal_valid) {
        nv_rm_driver_unwind();
        nv_rm_driver_fail(NV_RM_FAILURE_HAL, K_EIO);
        return K_EIO;
    }

    g_nv_gpu.state = NV_GPU_HAL_READY;
    liteos_serial_write_serial_only(
        "LITEOS_NVIDIA_PHASE4_HAL_READY BLACKWELL GB205\r\n");
    return K_OK;
}

const nv_gpu_device_t *nv_rm_driver_current(void)
{
    return &g_nv_gpu;
}
