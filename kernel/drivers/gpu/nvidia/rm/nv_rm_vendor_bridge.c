#include "nv_rm_vendor_bridge.h"

#include <nv.h>

#ifndef LITEOS_NVIDIA_PHASE5
#define LITEOS_NVIDIA_PHASE5 0
#endif

/* 595.84's immutable Linux MSI-X path caps the GPU at this many vectors
 * (NV_RM_MAX_MSIX_LINES).  The RM ISR is shared here because the vendor
 * entry point does not consume the Linux IRQ number. */
#define LITEOS_NVIDIA_PHASE5_MSIX_ENTRIES 8U

#include <arch/x86_64/cpu.h>
#include <kernel/console.h>
#include <kernel/deferred.h>
#include <kernel/mm.h>

#include <gpu/gpu.h>
#include <gpu/gpu_arch.h>
#include <core/hal.h>
#include <core/hal_mgr.h>
#include <core/system.h>
#include <gpu_mgr/gpu_mgr.h>
#include <mem_mgr/io_vaspace.h>
#include <nv-priv.h>
#include <nv_arch.h>
#include <nv_ref.h>
#include <nvoc/object.h>

#include "../../port/include/nv_port.h"

/* Keep the kernel-open OS interface header out of this translation unit.
 * Its rs_access.h and the RM NVOC headers are generated from distinct vendor
 * include roots and cannot be included together.  These are the only two
 * OS-interface entry points used by this bridge, declared with their exact
 * vendor ABI types. */
extern void *NV_API_CALL os_pci_init_handle(NvU32, NvU8, NvU8, NvU8,
                                             NvU16 *, NvU16 *);
extern void NV_API_CALL os_pci_remove(void *);

typedef struct nv_rm_vendor_context {
    nvidia_stack_t *stack;
    nv_state_t *state;
    void *pci_handle;
    nv_port_irq_t irq;
    nvidia_stack_t *isr_stack;
    nvidia_stack_t *isr_bh_stack;
    atomic_bool irq_bottom_pending;
    atomic_bool irq_bottom_queued;
    atomic_bool irq_bottom_schedule_failed;
    atomic_uint irq_top_hits;
    atomic_uint irq_rm_handled_hits;
    atomic_uint irq_bottom_hits;
    uint32_t irq_flags;
    bool irq_started;
    bool rm_initialized;
    bool event_locks_initialized;
    bool private_initialized;
    bool adapter_initialized;
    OBJGPU *phase4_gpu;
} nv_rm_vendor_context_t;

static void nv_rm_vendor_irq_bottom_half(void *argument);

static void nv_rm_vendor_schedule_irq_bottom_half(
    nv_rm_vendor_context_t *context)
{
    bool expected = false;

    if (context == NULL ||
        !atomic_compare_exchange_strong_explicit(
            &context->irq_bottom_queued, &expected, true,
            memory_order_acq_rel, memory_order_acquire))
        return;

    if (!deferred_try_schedule(nv_rm_vendor_irq_bottom_half, context)) {
        atomic_store_explicit(&context->irq_bottom_queued, false,
                              memory_order_release);
        atomic_store_explicit(&context->irq_bottom_schedule_failed, true,
                              memory_order_release);
    }
}

static void nv_rm_vendor_irq_top(void *argument)
{
    nv_rm_vendor_context_t *context =
        (nv_rm_vendor_context_t *)argument;
    NvU32 need_bottom_half = NV_FALSE;

    if (context == NULL || context->state == NULL ||
        context->isr_stack == NULL || !context->irq_started)
        return;

    atomic_fetch_add_explicit(&context->irq_top_hits, 1U,
                              memory_order_relaxed);

    /* rm_isr() is the vendor top-half entry.  It owns interrupt pending
     * inspection and returns the exact bottom-half requirement. */
    if (rm_isr(context->isr_stack, context->state, &need_bottom_half)) {
        atomic_fetch_add_explicit(&context->irq_rm_handled_hits, 1U,
                                  memory_order_relaxed);
    }
    if (need_bottom_half != NV_FALSE) {
        atomic_store_explicit(&context->irq_bottom_pending, true,
                              memory_order_release);
        nv_rm_vendor_schedule_irq_bottom_half(context);
    }
}

static void nv_rm_vendor_irq_bottom_half(void *argument)
{
    nv_rm_vendor_context_t *context =
        (nv_rm_vendor_context_t *)argument;

    if (context == NULL)
        return;

    /* Drain coalesced vendor requests before publishing the queue slot as
     * idle.  The final pending check closes the IRQ/worker hand-off race. */
    for (;;) {
        if (!atomic_exchange_explicit(&context->irq_bottom_pending, false,
                                      memory_order_acq_rel))
            break;
        atomic_fetch_add_explicit(&context->irq_bottom_hits, 1U,
                                  memory_order_relaxed);
        if (context->state != NULL && context->isr_bh_stack != NULL)
            rm_isr_bh(context->isr_bh_stack, context->state);
    }

    atomic_store_explicit(&context->irq_bottom_queued, false,
                          memory_order_release);
    if (atomic_load_explicit(&context->irq_bottom_pending,
                             memory_order_acquire))
        nv_rm_vendor_schedule_irq_bottom_half(context);
}

static void nv_rm_vendor_drain_irq_work(nv_rm_vendor_context_t *context)
{
    if (context == NULL)
        return;

    while (atomic_load_explicit(&context->irq_bottom_queued,
                                memory_order_acquire)) {
        /* RM probing currently precedes scheduler worker startup.  If an
         * early vendor path requested a bottom half, execute queued work in
         * the existing deferred runner while teardown keeps the RM state
         * alive. */
        if (!deferred_worker_started() && deferred_run(64U) != 0U)
            continue;
        __asm__ volatile ("pause");
    }
}

static void nv_rm_vendor_stop_irq(nv_rm_vendor_context_t *context)
{
    kstatus_t status;

    if (context == NULL || !context->irq_started)
        return;

    status = nv_port_irq_stop(&context->irq);
    nv_rm_vendor_drain_irq_work(context);

    if (context->irq.use_msix && context->irq.device != NULL) {
        kstatus_t disable = pci_msix_disable((pci_device_t *)
                                              context->irq.device);
        if (status == K_OK)
            status = disable;
    }

    if (status != K_OK)
        liteos_serial_printf_serial_only(
            "LITEOS_NVIDIA_IRQ_STOP_FAIL status=%d\r\n", (int)status);

    if (context->state != NULL)
        context->state->flags &= ~context->irq_flags;
    context->irq_flags = 0U;
    context->irq_started = false;
}

static kstatus_t nv_rm_vendor_start_irq(
    const pci_device_t *device, nv_rm_vendor_context_t *context)
{
    uint32_t apic_id;
    kstatus_t status = K_ENOENT;

    if (device == NULL || context == NULL || context->state == NULL)
        return K_EINVAL;

    apic_id = x86_current_apic_id();

    /* Match the vendor PCI path's MSI-X preference, but use one real vector
     * for the RM interrupt stream.  MSI remains an explicit fallback. */
    if (device->msix_capability != 0U && device->msix_table_size != 0U) {
        status = nv_port_irq_init(&context->irq, device, true, 0U, apic_id,
                                  nv_rm_vendor_irq_top, context, NULL, NULL);
        if (status == K_OK)
            status = nv_port_irq_start(&context->irq);
        if (status == K_OK) {
            /* NVIDIA's 595.84 Linux path enables all supported MSI-X table
             * entries before registering the RM ISR.  Keep the LiteOS
             * handler shared, but program every entry that the vendor path
             * would request so any GPU interrupt source can reach it. */
            paddr_t table = {0};
            uint16_t entries = 0U;
            uint16_t active_entries = 0U;
            if (pci_msix_table(device, &table, &entries) != K_OK ||
                entries == 0U) {
                (void)nv_port_irq_stop(&context->irq);
                (void)pci_msix_disable((pci_device_t *)device);
                status = K_EIO;
            } else {
                active_entries = entries < LITEOS_NVIDIA_PHASE5_MSIX_ENTRIES ?
                    entries : LITEOS_NVIDIA_PHASE5_MSIX_ENTRIES;
                for (uint16_t entry = 1U; entry < active_entries; ++entry) {
                    status = pci_msix_configure((pci_device_t *)device, entry,
                                                apic_id, context->irq.vector);
                    if (status != K_OK) break;
                }
                if (status != K_OK) {
                    (void)nv_port_irq_stop(&context->irq);
                    (void)pci_msix_disable((pci_device_t *)device);
                }
            }
        }
        if (status == K_OK) {
            context->irq_flags = NV_FLAG_USES_MSIX;
            context->state->flags |= context->irq_flags;
            context->irq_started = true;
            {
                paddr_t table = {0};
                uint16_t entries = 0U;
                uint64_t message_address = 0U;
                uint32_t message_data = 0U;
                uint32_t vector_control = 0U;
                if (pci_msix_table(device, &table, &entries) == K_OK &&
                    pci_msix_read_entry(device, context->irq.msix_entry,
                                        &message_address, &message_data,
                                        &vector_control) == K_OK) {
                    liteos_serial_printf_serial_only(
                        "LITEOS_NVIDIA_IRQ_CONFIG mode=msix table=%llx entries=%u "
                        "active=%u entry=%u addr=%llx data=%x ctl=%x apic=%u vector=%u\r\n",
                        (unsigned long long)table.value,
                        (unsigned)entries,
                        (unsigned)(entries < LITEOS_NVIDIA_PHASE5_MSIX_ENTRIES ?
                                   entries : LITEOS_NVIDIA_PHASE5_MSIX_ENTRIES),
                        (unsigned)context->irq.msix_entry,
                        (unsigned long long)message_address,
                        (unsigned)message_data,
                        (unsigned)vector_control,
                        (unsigned)context->irq.apic_id,
                        (unsigned)context->irq.vector);
                }
            }
            liteos_serial_printf_serial_only(
                "LITEOS_NVIDIA_IRQ_READY MSI-X vector=%u\r\n",
                (unsigned)context->irq.vector);
            return K_OK;
        }
        (void)pci_msix_disable((pci_device_t *)device);
    }

    if (device->msi_capability != 0U) {
        status = nv_port_irq_init(&context->irq, device, false, 0U,
                                  apic_id, nv_rm_vendor_irq_top, context,
                                  NULL, NULL);
        if (status == K_OK)
            status = nv_port_irq_start(&context->irq);
        if (status == K_OK) {
            context->irq_flags = NV_FLAG_USES_MSI;
            context->state->flags |= context->irq_flags;
            context->irq_started = true;
            liteos_serial_printf_serial_only(
                "LITEOS_NVIDIA_IRQ_READY MSI vector=%u\r\n",
                (unsigned)context->irq.vector);
            return K_OK;
        }
    }

    return status == K_ENOENT ? K_EIO : status;
}

static void nv_rm_vendor_cleanup(nv_rm_vendor_context_t *context)
{
    if (context == NULL)
        return;

    /* Match the immutable 595.84 Linux teardown order: disable the running
     * adapter while its RM interrupt path is still available, then tear down
     * the IRQ, and only then release the adapter state.  A failed
     * rm_init_adapter() is not a successful adapter and must not be passed to
     * rm_shutdown_adapter(). */
    if (context->adapter_initialized)
        rm_disable_adapter(context->stack, context->state);

    nv_rm_vendor_stop_irq(context);

    if (context->adapter_initialized)
        rm_shutdown_adapter(context->stack, context->state);

    /* Phase 4 keeps only the real vendor OBJGPU discovery object alive.  It
     * must be deleted before the private RM and global RM state are released;
     * GSP bootstrap and the full adapter path begin in Phase 5. */
    if (context->phase4_gpu != NULL) {
        objDelete(context->phase4_gpu);
        context->phase4_gpu = NULL;
    }

    if (context->private_initialized)
        rm_free_private_state(context->stack, context->state);

    if (context->event_locks_initialized)
        rm_destroy_event_locks(context->stack, context->state);

    if (context->rm_initialized)
        rm_shutdown_rm(context->stack);

    if (context->pci_handle != NULL) {
        os_pci_remove(context->pci_handle);
        context->pci_handle = NULL;
    }

    if (context->state != NULL) {
        kfree(context->state);
        context->state = NULL;
    }

    if (context->isr_bh_stack != NULL) {
        kfree(context->isr_bh_stack);
        context->isr_bh_stack = NULL;
    }

    if (context->isr_stack != NULL) {
        kfree(context->isr_stack);
        context->isr_stack = NULL;
    }

    if (context->stack != NULL) {
        kfree(context->stack);
        context->stack = NULL;
    }

    kfree(context);
}

static void nv_rm_vendor_fill_aperture(nv_aperture_t *aperture,
                                       const pci_bar_t *bar,
                                       uint32_t bar_index)
{
    aperture->cpu_address = bar->address;
    aperture->size = bar->length;
    aperture->offset = (NvU32)(0x10U + (bar_index * 4U));
    aperture->map = NULL;
    aperture->map_u = 0;
}

static kstatus_t nv_rm_vendor_fill_pci(const pci_device_t *device,
                                       nv_rm_vendor_context_t *context)
{
    nv_state_t *state = context->state;
    uint32_t memory_bar = 0U;
    uint32_t i;
    uint32_t subsystem;
    uint8_t interrupt_line;
    NvU16 vendor = 0U;
    NvU16 device_id = 0U;

    state->pci_info.domain = (NvU32)device->segment;
    state->pci_info.bus = device->bus;
    state->pci_info.slot = device->slot;
    state->pci_info.function = device->function;
    state->pci_info.vendor_id = device->vendor_id;
    state->pci_info.device_id = device->device_id;

    for (i = 0U; i < NVRM_PCICFG_NUM_DWORDS; ++i) {
        kstatus_t status = pci_read_config32(device, (uint16_t)(i * 4U),
                                              &state->pci_cfg_space[i]);
        if (status != K_OK)
            return status;
    }

    subsystem = state->pci_cfg_space[0x2cU / 4U];
    state->subsystem_vendor = (NvU16)(subsystem & 0xffffU);
    state->subsystem_id = (NvU16)(subsystem >> 16U);

    if (pci_read_config8(device, 0x3cU, &interrupt_line) == K_OK &&
        interrupt_line != 0xffU)
        state->interrupt_line = interrupt_line;

    for (i = 0U; i < PCI_MAX_BARS && memory_bar < NV_GPU_NUM_BARS; ++i) {
        const pci_bar_t *bar = &device->bars[i];

        if ((bar->flags & PCI_RESOURCE_MEMORY) == 0U ||
            bar->length == 0U || bar->address == 0U)
            continue;

        nv_rm_vendor_fill_aperture(&state->bars[memory_bar], bar, i);
        ++memory_bar;
    }

    if (memory_bar < 2U)
        return K_EIO;

    state->regs = &state->bars[0];
    state->fb = &state->bars[1];
    /* LiteOS has no GPU IOVA-remap domain at this boundary.  Zero is a
     * valid vendor vaspace id, not the sentinel for a direct-physical PCIe
     * path; leave GMMU/VA ownership with NVIDIA by passing the exact vendor
     * "no domain" value. */
    state->iovaspace_id = NV_IOVA_DOMAIN_NONE;
    state->cpu_numa_node_id = (NvS32)-1;
#if LITEOS_NVIDIA_PHASE5
    state->request_firmware = NV_TRUE;
#else
    state->request_firmware = NV_FALSE;
#endif
    state->request_fw_client_rm = NV_FALSE;
    state->allow_fallback_to_monolithic_rm = NV_FALSE;
    state->enable_firmware_logs = NV_FALSE;
    state->os_state = context;

    context->pci_handle = os_pci_init_handle(
        state->pci_info.domain, state->pci_info.bus, state->pci_info.slot,
        state->pci_info.function, &vendor, &device_id);
    if (context->pci_handle == NULL)
        return K_EIO;

    if (vendor != state->pci_info.vendor_id ||
        device_id != state->pci_info.device_id)
        return K_EIO;

    state->handle = context->pci_handle;
    return K_OK;
}

static void nv_rm_vendor_log_boot_registers(const nv_state_t *state)
{
    nv_port_mmio_mapping_t mapping = {0};

    if (state == NULL || state->regs == NULL)
        return;

    if (nv_port_mmio_map(paddr_make(state->regs->cpu_address), 0x1000U,
                         NV_PORT_CACHE_UC, &mapping) != K_OK) {
        liteos_serial_write_serial_only(
            "LITEOS_NVIDIA_BOOT_REGS_READ_FAILED\r\n");
        return;
    }

    liteos_serial_printf_serial_only(
        "LITEOS_NVIDIA_BOOT_REGS boot0=%x boot1=%x boot42=%x\r\n",
        (unsigned)nv_port_mmio_read32(mapping.base, 0x000U),
        (unsigned)nv_port_mmio_read32(mapping.base, 0x004U),
        (unsigned)nv_port_mmio_read32(mapping.base, 0xa00U));
    (void)nv_port_mmio_unmap(&mapping);
}

static void nv_rm_vendor_log_hal_state(const nv_state_t *state)
{
    const nv_priv_t *private_state;
    OBJSYS *sys;
    OBJHALMGR *hal_mgr;
    OBJGPUMGR *gpu_mgr;
    GpuArch *gpu_arch = NULL;
    NvU64 encoded_dbdf = 0ULL;
    NvU32 probed_gpu_matches = 0U;
    NvU32 probed_dbdf_matches = 0U;
    NvU32 registered_gpu_id = 0U;
    NvU64 registered_dbdf = 0ULL;
    NvU32 matched_impl = 0U;
    NV_STATUS match_status = (NV_STATUS)0xffffffffU;

    if (state == NULL)
        return;

    private_state = (const nv_priv_t *)NV_GET_NV_PRIV(state);
    sys = SYS_GET_INSTANCE();
    hal_mgr = sys != NULL ? SYS_GET_HALMGR(sys) : NULL;
    gpu_mgr = sys != NULL ? SYS_GET_GPUMGR(sys) : NULL;
    if (private_state != NULL && hal_mgr != NULL) {
        match_status = halmgrGetHalForGpu(
            hal_mgr, private_state->pmc_boot_0,
            private_state->pmc_boot_42, &matched_impl);
    }
    encoded_dbdf = gpuEncodeDomainBusDevice(
        state->pci_info.domain, state->pci_info.bus, state->pci_info.slot);
    if (private_state != NULL && gpu_mgr != NULL) {
        gpu_arch = gpumgrGetGpuArch(private_state->pmc_boot_42, 0U,
                                    TEGRA_CHIP_TYPE_DEFAULT);
        for (NvU32 i = 0U; i < NV_ARRAY_ELEMENTS(gpu_mgr->probedGpus); ++i) {
            if (gpu_mgr->probedGpus[i].gpuId == state->gpu_id) {
                ++probed_gpu_matches;
                registered_gpu_id = gpu_mgr->probedGpus[i].gpuId;
                registered_dbdf = gpu_mgr->probedGpus[i].gpuDomainBusDevice;
            }
            if (gpu_mgr->probedGpus[i].gpuDomainBusDevice == encoded_dbdf)
                ++probed_dbdf_matches;
        }
    }

    liteos_serial_printf_serial_only(
        "LITEOS_NVIDIA_HAL_STATE sys=%llx mgr=%llx gb205=%llx match=%x impl=%x dbdf=%llx gpu=%x probed=%u/%u reg=%x/%llx arch=%llx chip=%x/%x\r\n",
        (unsigned long long)(uintptr_t)sys,
        (unsigned long long)(uintptr_t)hal_mgr,
        hal_mgr != NULL
            ? (unsigned long long)(uintptr_t)hal_mgr->pHalList[HAL_IMPL_GB205]
            : 0ULL,
        (unsigned)match_status, (unsigned)matched_impl,
        (unsigned long long)encoded_dbdf, private_state != NULL
            ? (unsigned)state->gpu_id : 0U,
        (unsigned)probed_gpu_matches, (unsigned)probed_dbdf_matches,
        (unsigned)registered_gpu_id, (unsigned long long)registered_dbdf,
        (unsigned long long)(uintptr_t)gpu_arch,
        gpu_arch != NULL ? (unsigned)gpu_arch->chipArch : 0U,
        gpu_arch != NULL ? (unsigned)gpu_arch->chipImpl : 0U);
}

/* Phase 4 stops at the real vendor OBJGPU/NVOC/HAL discovery boundary.  The
 * object is not inserted into gpumgr's handle table and is deleted by the
 * normal reverse teardown path.  Phase 5 owns the adapter/GSP transition. */
static kstatus_t nv_rm_vendor_create_phase4_gpu(
    nv_rm_vendor_context_t *context)
{
    const nv_priv_t *private_state;
    OBJSYS *sys;
    GpuArch *gpu_arch;
    OBJGPU *probe_gpu = NULL;
    NV_STATUS create_status;
    NV_STATUS bind_status = (NV_STATUS)0xffffffffU;

    if (context == NULL || context->state == NULL)
        return K_EINVAL;

    private_state = (const nv_priv_t *)NV_GET_NV_PRIV(context->state);
    sys = SYS_GET_INSTANCE();
    if (private_state == NULL || sys == NULL)
        return K_EIO;

    gpu_arch = gpumgrGetGpuArch(private_state->pmc_boot_42, 0U,
                                TEGRA_CHIP_TYPE_DEFAULT);
    if (gpu_arch == NULL)
        return K_EIO;

    /* 595.84's generated host NVOC graph maps the physical-PF discovery
     * variant to PF_KERNEL_ONLY.  This selects the vendor GB205 HAL only; it
     * does not load firmware, start GSP, or issue an RPC. */
    create_status = objCreate(&probe_gpu, sys, OBJGPU,
                              gpu_arch->chipArch,
                              gpu_arch->chipImpl,
                              gpu_arch->hidrev,
                              gpu_arch->tegraType,
                              RM_RUNTIME_VARIANT_PF_KERNEL_ONLY,
                              0U,
                              0U,
                              context->state->gpu_id,
                              NULL,
                              gpu_arch);
    if (create_status == NV_OK && probe_gpu != NULL)
        bind_status = gpuBindHalLegacy(probe_gpu,
                                       private_state->pmc_boot_0,
                                       private_state->pmc_boot_42,
                                       0U);

    liteos_serial_printf_serial_only(
        "LITEOS_NVIDIA_PHASE4_GPU_OBJECT create=%x bind=%x obj=%llx hal=%llx impl=%x arch=%x chip=%x\r\n",
        (unsigned)create_status,
        (unsigned)bind_status,
        (unsigned long long)(uintptr_t)probe_gpu,
        probe_gpu != NULL ? (unsigned long long)(uintptr_t)probe_gpu->pHal : 0ULL,
        probe_gpu != NULL ? (unsigned)probe_gpu->halImpl : 0U,
        (unsigned)gpu_arch->chipArch, (unsigned)gpu_arch->chipImpl);

    if (create_status != NV_OK || bind_status != NV_OK || probe_gpu == NULL) {
        if (probe_gpu != NULL)
            objDelete(probe_gpu);
        return K_EIO;
    }

    context->phase4_gpu = probe_gpu;
    liteos_serial_write_serial_only(
        "LITEOS_NVIDIA_PHASE4_GPU_OBJECT_READY\r\n");
    return K_OK;
}

static void nv_rm_vendor_set_result(nv_rm_vendor_context_t *context,
                                    nv_rm_vendor_result_t *result)
{
    nv_priv_t *private_state = (nv_priv_t *)NV_GET_NV_PRIV(context->state);
    /* Phase 4 deliberately reports the real discovery object constructed by
     * nv_rm_vendor_create_phase4_gpu().  private_state->pGpu belongs to the
     * later adapter path and may be the vendor's pre-attach placeholder; it
     * is not the object whose PF_KERNEL_ONLY NVOC/HAL binding we just proved. */
    OBJGPU *gpu = context != NULL ? context->phase4_gpu : NULL;
    GpuArch *gpu_arch = gpu != NULL ? gpu->pGpuArch : NULL;

    if (result == NULL)
        return;

    *result = (nv_rm_vendor_result_t){0};
    result->context = context;
    result->rm_gpu = gpu;
    result->rm_device = gpu != NULL && gpu->hInternalDevice != 0U
                            ? (void *)(uintptr_t)gpu->hInternalDevice
                            : NULL;

    if (gpu == NULL || gpu_arch == NULL)
        return;

    /* chipInfo is populated by gpuPostConstruct, which is intentionally a
     * later Phase 5 boundary.  The architecture facts used here come from
     * the vendor GpuArch selected by the immutable HAL manager. */
    result->chip_arch = gpu_arch->chipArch;
    result->chip_impl = gpu_arch->chipImpl;
    result->hal_impl = gpu->halImpl;
    /* GpuArch stores the PMC_BOOT_42 architecture code (0x1b for GB200),
     * while GPU_ARCHITECTURE_BLACKWELL_GB2XX is the separate published
     * architecture encoding (0x1b0).  Use the value actually returned by the
     * immutable vendor HAL lookup at this boundary. */
    result->blackwell = result->chip_arch == NV_PMC_BOOT_42_ARCHITECTURE_GB200;
    result->gb205 = result->chip_impl == GPU_IMPLEMENTATION_GB205;
    /* The private-state HAL flags are set by the later adapter path.  Phase 4
     * stops after this real OBJGPU NVOC/HAL binding, so the vendor HAL object
     * and its resolved GB205 implementation are the valid evidence here. */
    result->hal_valid = gpu->pHal != NULL && gpu->halImpl == HAL_IMPL_GB205;
    liteos_serial_printf_serial_only(
        "LITEOS_NVIDIA_PHASE4_RESULT gpu=%llx arch=%llx hal=%llx pga=%llx impl=%x flags=%x valid=%u blackwell=%u gb205=%u\r\n",
        (unsigned long long)(uintptr_t)gpu,
        (unsigned long long)(uintptr_t)gpu_arch,
        (unsigned long long)(uintptr_t)gpu->pHal,
        (unsigned long long)(uintptr_t)gpu->pGpuArch,
        (unsigned)gpu->halImpl,
        private_state != NULL ? (unsigned)private_state->flags : 0U,
        result->hal_valid ? 1U : 0U,
        result->blackwell ? 1U : 0U,
        result->gb205 ? 1U : 0U);
}

kstatus_t nv_rm_vendor_start(const pci_device_t *device,
                             void **context_out,
                             nv_rm_vendor_result_t *result)
{
    nv_rm_vendor_context_t *context;
    NV_STATUS vendor_status;

    if (context_out == NULL || result == NULL || device == NULL)
        return K_EINVAL;

    *context_out = NULL;
    *result = (nv_rm_vendor_result_t){0};

    context = (nv_rm_vendor_context_t *)kzalloc(sizeof(*context), 0U);
    if (context == NULL)
        return K_ENOMEM;

    context->stack = (nvidia_stack_t *)kzalloc(sizeof(*context->stack), 0U);
    context->state = (nv_state_t *)kzalloc(sizeof(*context->state), 0U);
    context->isr_stack = (nvidia_stack_t *)kzalloc(
        sizeof(*context->isr_stack), 0U);
    context->isr_bh_stack = (nvidia_stack_t *)kzalloc(
        sizeof(*context->isr_bh_stack), 0U);
    atomic_init(&context->irq_bottom_pending, false);
    atomic_init(&context->irq_bottom_queued, false);
    atomic_init(&context->irq_bottom_schedule_failed, false);
    atomic_init(&context->irq_top_hits, 0U);
    atomic_init(&context->irq_rm_handled_hits, 0U);
    atomic_init(&context->irq_bottom_hits, 0U);
    if (context->stack == NULL || context->state == NULL ||
        context->isr_stack == NULL || context->isr_bh_stack == NULL) {
        nv_rm_vendor_cleanup(context);
        return K_ENOMEM;
    }

    context->isr_stack->size = (NvU32)sizeof(context->isr_stack->stack);
    context->isr_stack->top =
        context->isr_stack->stack + context->isr_stack->size;
    context->isr_bh_stack->size =
        (NvU32)sizeof(context->isr_bh_stack->stack);
    context->isr_bh_stack->top =
        context->isr_bh_stack->stack + context->isr_bh_stack->size;

    context->stack->size = (NvU32)sizeof(context->stack->stack);
    context->stack->top = context->stack->stack + context->stack->size;

    if (nv_rm_vendor_fill_pci(device, context) != K_OK) {
        nv_rm_vendor_cleanup(context);
        return K_EIO;
    }

    if (!rm_init_rm(context->stack)) {
        nv_rm_vendor_cleanup(context);
        return K_EIO;
    }
    context->rm_initialized = true;

    /* The Linux frontend initializes these per-device RM locks before
     * rm_is_supported_device().  LiteOS cannot call nv_lock_init_locks(),
     * because that wrapper owns Linux-only nv_linux_state_t storage; the
     * vendor RM event-lock entry points are the portable part of the same
     * lifecycle and operate directly on nv_state_t. */
    if (!rm_init_event_locks(context->stack, context->state)) {
        liteos_serial_write_serial_only(
            "LITEOS_NVIDIA_EVENT_LOCKS_FAILED\r\n");
        nv_rm_vendor_cleanup(context);
        return K_EIO;
    }
    context->event_locks_initialized = true;

    vendor_status = rm_is_supported_device(context->stack, context->state);
    if (vendor_status != NV_OK) {
        liteos_serial_printf_serial_only(
            "LITEOS_NVIDIA_RM_UNSUPPORTED status=%x\r\n",
            (unsigned)vendor_status);
        nv_rm_vendor_cleanup(context);
        return K_EIO;
    }

    if (!rm_init_private_state(context->stack, context->state)) {
        nv_rm_vendor_cleanup(context);
        return K_EIO;
    }
    context->private_initialized = true;

    nv_rm_vendor_log_boot_registers(context->state);
    nv_rm_vendor_log_hal_state(context->state);

    if (nv_rm_vendor_create_phase4_gpu(context) != K_OK) {
        liteos_serial_write_serial_only(
            "LITEOS_NVIDIA_PHASE4_GPU_OBJECT_FAIL\r\n");
        nv_rm_vendor_cleanup(context);
        return K_EIO;
    }

    nv_rm_vendor_set_result(context, result);
    if (!result->hal_valid) {
        nv_rm_vendor_cleanup(context);
        *result = (nv_rm_vendor_result_t){0};
        return K_EIO;
    }

    liteos_serial_write_serial_only(
        "LITEOS_NVIDIA_PHASE4_HAL_READY\r\n");
#if LITEOS_NVIDIA_PHASE5
    if (nv_rm_vendor_start_irq(device, context) != K_OK) {
        liteos_serial_write_serial_only("LITEOS_NVIDIA_GSP_IRQ_FAIL\r\n");
        nv_rm_vendor_cleanup(context);
        *result = (nv_rm_vendor_result_t){0};
        return K_EIO;
    }
    liteos_serial_write_serial_only("LITEOS_NVIDIA_GSP_BOOT_START\r\n");
    if (!rm_init_adapter(context->stack, context->state)) {
        liteos_serial_printf_serial_only(
            "LITEOS_NVIDIA_IRQ_DIAG top=%u handled=%u bottom=%u mode=%s vector=%u\r\n",
            (unsigned)atomic_load_explicit(&context->irq_top_hits,
                                           memory_order_acquire),
            (unsigned)atomic_load_explicit(&context->irq_rm_handled_hits,
                                           memory_order_acquire),
            (unsigned)atomic_load_explicit(&context->irq_bottom_hits,
                                           memory_order_acquire),
            context->irq.use_msix ? "msix" : "msi",
            (unsigned)context->irq.vector);
        liteos_serial_write_serial_only("LITEOS_NVIDIA_GSP_BOOT_FAIL\r\n");
        nv_rm_vendor_cleanup(context);
        *result = (nv_rm_vendor_result_t){0};
        return K_EIO;
    }
    context->adapter_initialized = true;
    liteos_serial_write_serial_only(
        "LITEOS_NVIDIA_GSP_BOOT_FINISH status=ready\r\n");
#endif
    *context_out = context;
    return K_OK;
}

void nv_rm_vendor_stop(void *opaque_context)
{
    nv_rm_vendor_cleanup((nv_rm_vendor_context_t *)opaque_context);
}
