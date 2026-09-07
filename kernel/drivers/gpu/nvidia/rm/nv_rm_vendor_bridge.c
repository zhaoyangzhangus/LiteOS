#include "nv_rm_vendor_bridge.h"

#include <nv.h>

#include <arch/x86_64/cpu.h>
#include <kernel/console.h>
#include <kernel/deferred.h>
#include <kernel/mm.h>

#include <gpu/gpu.h>
#include <gpu/gpu_arch.h>
#include <nv-priv.h>
#include <nv_arch.h>

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
    uint32_t irq_flags;
    bool irq_started;
    bool rm_initialized;
    bool private_initialized;
    bool adapter_attempted;
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

    /* rm_isr() is the vendor top-half entry.  It owns interrupt pending
     * inspection and returns the exact bottom-half requirement. */
    if (rm_isr(context->isr_stack, context->state, &need_bottom_half) &&
        need_bottom_half != NV_FALSE) {
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
            context->irq_flags = NV_FLAG_USES_MSIX;
            context->state->flags |= context->irq_flags;
            context->irq_started = true;
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

    nv_rm_vendor_stop_irq(context);

    if (context->adapter_attempted)
        rm_shutdown_adapter(context->stack, context->state);

    if (context->private_initialized)
        rm_free_private_state(context->stack, context->state);

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
    state->cpu_numa_node_id = (NvS32)-1;
    state->request_firmware = NV_FALSE;
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

static void nv_rm_vendor_set_result(nv_rm_vendor_context_t *context,
                                    nv_rm_vendor_result_t *result)
{
    nv_priv_t *private_state = (nv_priv_t *)NV_GET_NV_PRIV(context->state);
    OBJGPU *gpu = private_state != NULL ? private_state->pGpu : NULL;
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

    result->chip_arch = gpu->chipInfo.platformId;
    result->chip_impl = gpu->chipInfo.implementationId;
    result->hal_impl = gpu->halImpl;
    result->blackwell = result->chip_arch == GPU_ARCHITECTURE_BLACKWELL_GB2XX;
    result->gb205 = result->chip_impl == GPU_IMPLEMENTATION_GB205;
    result->hal_valid = result->rm_device != NULL && gpu->pHal != NULL &&
                        (private_state->flags & NV_INIT_FLAG_HAL) != 0U &&
                        (private_state->flags & NV_INIT_FLAG_HAL_COMPONENTS) != 0U;
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

    vendor_status = rm_is_supported_device(context->stack, context->state);
    if (vendor_status != NV_OK) {
        liteos_serial_printf_serial_only(
            "LITEOS_NVIDIA_RM_UNSUPPORTED status=%08x\r\n",
            (unsigned)vendor_status);
        nv_rm_vendor_cleanup(context);
        return K_EIO;
    }

    if (!rm_init_private_state(context->stack, context->state)) {
        nv_rm_vendor_cleanup(context);
        return K_EIO;
    }
    context->private_initialized = true;

    if (nv_rm_vendor_start_irq(device, context) != K_OK) {
        liteos_serial_write_serial_only(
            "LITEOS_NVIDIA_IRQ_UNAVAILABLE\r\n");
        nv_rm_vendor_cleanup(context);
        return K_EIO;
    }

    /*
     * The call is intentionally the vendor adapter entry point.  LiteOS
     * supplies no GSP protocol, VRAM PMA, GPU VA, or GMMU implementation;
     * any such work remains inside the immutable RM object graph.
     */
    context->adapter_attempted = true;
    if (!rm_init_adapter(context->stack, context->state)) {
        nv_rm_vendor_cleanup(context);
        return K_EIO;
    }

    if (atomic_load_explicit(&context->irq_bottom_schedule_failed,
                             memory_order_acquire)) {
        nv_rm_vendor_cleanup(context);
        return K_EIO;
    }

    nv_rm_vendor_set_result(context, result);
    if (!result->hal_valid) {
        nv_rm_vendor_cleanup(context);
        *result = (nv_rm_vendor_result_t){0};
        return K_EIO;
    }

    *context_out = context;
    return K_OK;
}

void nv_rm_vendor_stop(void *opaque_context)
{
    nv_rm_vendor_cleanup((nv_rm_vendor_context_t *)opaque_context);
}
