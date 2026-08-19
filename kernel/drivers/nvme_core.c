#include <arch/x86_64/apic.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/paging.h>
#include <kernel/block.h>
#include <kernel/dma.h>
#include <kernel/deferred.h>
#include <kernel/irq.h>
#include <kernel/io.h>
#include <kernel/kmem.h>
#include <kernel/nvme_core.h>

#define NVME_CLASS_MASS_STORAGE 0x01U
#define NVME_SUBCLASS_NVM        0x08U
#define NVME_PROGIF_NVM          0x02U

#define NVME_REG_CAP             0x000U
#define NVME_REG_VS              0x008U
#define NVME_REG_CC              0x014U
#define NVME_REG_CSTS            0x01CU
#define NVME_REG_AQA             0x024U
#define NVME_REG_ASQ             0x028U
#define NVME_REG_ACQ             0x030U
#define NVME_REG_DBS             0x1000U

#define NVME_CC_ENABLE           (1U << 0)
#define NVME_CSTS_READY          (1U << 0)
#define NVME_CSTS_FATAL          (1U << 1)
#define NVME_ADMIN_IDENTIFY      0x06U
#define NVME_ADMIN_SET_FEATURES  0x09U
#define NVME_ADMIN_CREATE_CQ     0x05U
#define NVME_ADMIN_CREATE_SQ     0x01U
#define NVME_FEATURE_NUMBER_QUEUES 0x07U
#define NVME_IO_FLUSH            0x00U
#define NVME_IO_WRITE            0x01U
#define NVME_IO_READ             0x02U
#define NVME_MMIO_BASE           (X86_64_MMIO_BASE + 0x01000000ULL)
#define NVME_MMIO_SLOT_SIZE      0x00100000ULL
#define NVME_MAX_CONTROLLERS     8U
#define NVME_COMPLETION_STOPPING 0x80000000U
#define NVME_COMPLETION_REF_MASK 0x7FFFFFFFU

static driver_t g_nvme_driver;
static nvme_controller_t *g_nvme_controllers[NVME_MAX_CONTROLLERS];
static atomic_uint g_nvme_driver_state;
static uint32_t g_nvme_mmio_slots;
static bool g_nvme_hardware_seen;
static kstatus_t g_nvme_last_status;
static uint32_t g_nvme_last_stage;
static uint16_t g_nvme_last_completion;

typedef struct nvme_deferred_dma_cleanup {
    dma_mapping_t *mapping;
    page_t *page;
} nvme_deferred_dma_cleanup_t;

/*
 * 一个 pending 对象代表一个已经发布到 NVMe SQ 的命令。
 * 请求对象和 DMA 映射都由它持有到 CQ 完成，取消不能提前释放映射。
 */
typedef struct nvme_pending_io {
    list_head_t node;
    nvme_queue_t *queue;
    io_request_t *request;
    dma_mapping_t *mapping;
    uint64_t bytes;
    uint16_t command_id;
    bool cancelled;
} nvme_pending_io_t;

static void nvme_deferred_complete(void *argument);
static bool nvme_schedule_queue_completion(nvme_queue_t *queue);
static void nvme_abort_queue_pending(nvme_queue_t *queue, kstatus_t status);

static bool nvme_completion_ref_get(nvme_queue_t *queue) {
    unsigned value;
    if (queue == 0) return false;
    value = atomic_load_explicit(&queue->completion_work_refs,
                                 memory_order_acquire);
    for (;;) {
        if ((value & NVME_COMPLETION_STOPPING) != 0U ||
            (value & NVME_COMPLETION_REF_MASK) == NVME_COMPLETION_REF_MASK) {
            return false;
        }
        if (atomic_compare_exchange_weak_explicit(&queue->completion_work_refs,
                                                  &value, value + 1U,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            return true;
        }
    }
}

static void nvme_completion_ref_put(nvme_queue_t *queue) {
    if (queue == 0) return;
    atomic_fetch_sub_explicit(&queue->completion_work_refs, 1U,
                              memory_order_release);
}

static void nvme_stop_completion_work(nvme_queue_t *queue) {
    unsigned value;
    if (queue == 0) return;
    atomic_fetch_or_explicit(&queue->completion_work_refs,
                             NVME_COMPLETION_STOPPING, memory_order_acq_rel);
    for (;;) {
        value = atomic_load_explicit(&queue->completion_work_refs,
                                     memory_order_acquire);
        if ((value & NVME_COMPLETION_REF_MASK) == 0U) return;
        /* 正常卸载路径允许消费已经排队的 deferred 工作，避免永久等待。 */
        (void)deferred_run(8U);
        __asm__ volatile ("pause");
    }
}

static void nvme_retry_dma_cleanup(void *argument) {
    nvme_deferred_dma_cleanup_t *cleanup =
        (nvme_deferred_dma_cleanup_t *)argument;
    if (cleanup == 0 || cleanup->mapping == 0) return;
    if (dma_unmap_checked(cleanup->mapping) == K_OK) {
        if (cleanup->page != 0) page_free(cleanup->page);
        kfree(cleanup->mapping);
        kfree(cleanup);
        return;
    }
    /* 队列满时保留 cleanup；映射仍由 DMA 注册表和该堆对象持有。 */
    (void)deferred_schedule(nvme_retry_dma_cleanup, cleanup);
}

static kstatus_t nvme_release_transient_dma(dma_mapping_t *mapping,
                                             page_t *page) {
    if (mapping == 0) return K_EINVAL;
    kstatus_t status = dma_unmap_checked(mapping);
    if (status == K_OK) {
        if (page != 0) page_free(page);
        kfree(mapping);
        return K_OK;
    }
    nvme_deferred_dma_cleanup_t *cleanup =
        (nvme_deferred_dma_cleanup_t *)kzalloc(sizeof(*cleanup), 0);
    if (cleanup != 0) {
        cleanup->mapping = mapping;
        cleanup->page = page;
        if (deferred_schedule(nvme_retry_dma_cleanup, cleanup)) return status;
        /* 调度失败时故意不释放 cleanup；它仍保存着安全的持有者。 */
    }
    return status;
}

static kstatus_t nvme_record_status(kstatus_t status) {
    g_nvme_last_status = status;
    return status;
}

static void nvme_lock(spinlock_t *lock) {
    while (atomic_exchange_explicit(&lock->state, 1U,
                                    memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
}

static void nvme_unlock(spinlock_t *lock) {
    atomic_store_explicit(&lock->state, 0U, memory_order_release);
}

static void nvme_msix_handler(uint8_t vector, struct arch_trap_frame *frame,
                              void *context) {
    (void)frame;
    nvme_controller_t *controller = (nvme_controller_t *)context;
    if (controller == 0 || !controller->started) return;
    /* 硬中断只根据向量找到队列并投递 deferred，不消费 CQ、不分配内存。 */
    for (uint16_t index = 0; index < controller->io_queue_count; ++index) {
        nvme_queue_t *queue = &controller->io_queues[index];
        if (queue->active && queue->irq_vector == vector) {
            (void)nvme_schedule_queue_completion(queue);
            return;
        }
    }
    /* 硬中断只投递 deferred 工作，CQ 在普通内核上下文中有界消费。 */
}

static bool nvme_schedule_queue_completion(nvme_queue_t *queue) {
    bool expected = false;
    bool ref_acquired;
    if (queue == 0 || !queue->active) {
        return false;
    }
    ref_acquired = nvme_completion_ref_get(queue);
    if (!ref_acquired ||
        !atomic_compare_exchange_strong_explicit(&queue->completion_queued,
                                                 &expected, true,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        if (ref_acquired) nvme_completion_ref_put(queue);
        return false;
    }
    if (!deferred_try_schedule(nvme_deferred_complete, queue)) {
        atomic_store_explicit(&queue->completion_queued, false,
                              memory_order_release);
        nvme_completion_ref_put(queue);
        return false;
    }
    return true;
}

static uint32_t nvme_read32(const nvme_controller_t *controller, uint32_t offset) {
    volatile const uint32_t *address =
        (volatile const uint32_t *)(controller->registers + offset);
    uint32_t value;

    /*
     * A plain volatile uint32_t load is not sufficient here: when the
     * caller only tests a low bit, GCC may fold it into an 8-bit test.  NVMe
     * controller registers require 32-bit MMIO transactions (QEMU rejects
     * the narrower access), so keep the access width explicit.
     */
    __asm__ volatile ("movl %1, %0" : "=r"(value) : "m"(*address) : "memory");
    return value;
}

static uint64_t nvme_read64(const nvme_controller_t *controller, uint32_t offset) {
    uint32_t low = nvme_read32(controller, offset);
    uint32_t high = nvme_read32(controller, offset + 4U);
    return ((uint64_t)high << 32) | low;
}

static void nvme_write32(const nvme_controller_t *controller, uint32_t offset,
                         uint32_t value) {
    volatile uint32_t *address = (volatile uint32_t *)(controller->registers + offset);
    __asm__ volatile ("movl %1, %0" : "=m"(*address) : "r"(value) : "memory");
}

static void nvme_write64(const nvme_controller_t *controller, uint32_t offset,
                         uint64_t value) {
    nvme_write32(controller, offset, (uint32_t)value);
    nvme_write32(controller, offset + 4U, (uint32_t)(value >> 32));
}

static bool nvme_deadline_reached(uint64_t deadline) {
    return (int64_t)(x86_read_tsc() - deadline) >= 0;
}

static uint64_t nvme_timeout_deadline(const nvme_controller_t *controller) {
    uint64_t units = (controller->capabilities >> 24) & 0xFFU;
    uint64_t timeout_ns = units == 0 ? 5000000000ULL : units * 500000000ULL;
    uint64_t ticks = x86_timeout_ns_to_tsc(timeout_ns);
    uint64_t now = x86_read_tsc();
    return ticks > UINT64_MAX - now ? UINT64_MAX : now + ticks;
}

static kstatus_t nvme_wait_ready(const nvme_controller_t *controller, bool ready) {
    uint64_t deadline = nvme_timeout_deadline(controller);
    for (;;) {
        uint32_t status = nvme_read32(controller, NVME_REG_CSTS);
        if ((status & NVME_CSTS_FATAL) != 0) return K_EIO;
        if (((status & NVME_CSTS_READY) != 0) == ready) return K_OK;
        if (nvme_deadline_reached(deadline)) return K_ETIMEDOUT;
        __asm__ volatile ("pause");
    }
}

static void nvme_unmap_registers(nvme_controller_t *controller) {
    if (controller == 0 || controller->registers_va == 0) return;
    paddr_t root = x86_current_root_table();
    uint64_t pages = (controller->registers_size + PAGE_SIZE - 1ULL) >> PAGE_SHIFT;
    for (uint64_t i = 0; i < pages; ++i) {
        (void)x86_unmap_page(root, controller->registers_va + i * PAGE_SIZE, 0);
    }
    controller->registers = 0;
    controller->registers_va = 0;
    controller->registers_size = 0;
}

static kstatus_t nvme_map_registers(nvme_controller_t *controller) {
    if (controller == 0 || controller->pci == 0 ||
        controller->pci->bars[0].address == 0 || controller->pci->bars[0].length < PAGE_SIZE) {
        return K_EINVAL;
    }
    uint64_t offset = controller->pci->bars[0].address & (PAGE_SIZE - 1ULL);
    uint64_t size = controller->pci->bars[0].length + offset;
    size = (size + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
    if (size == 0 || size > NVME_MMIO_SLOT_SIZE ||
        g_nvme_mmio_slots >= NVME_MAX_CONTROLLERS) return K_EINVAL;
    vaddr_t virtual_base = NVME_MMIO_BASE +
                           (uint64_t)g_nvme_mmio_slots++ * NVME_MMIO_SLOT_SIZE;
    paddr_t root = x86_current_root_table();
    uint64_t physical_base = controller->pci->bars[0].address & ~(PAGE_SIZE - 1ULL);
    uint64_t pages = size >> PAGE_SHIFT;
    for (uint64_t i = 0; i < pages; ++i) {
        kstatus_t status = x86_map_page(root, virtual_base + i * PAGE_SIZE,
                                        paddr_make(physical_base + i * PAGE_SIZE),
                                        X86_PAGE_WRITE | X86_PAGE_GLOBAL, X86_CACHE_UC);
        if (status != K_OK) {
            for (uint64_t j = 0; j < i; ++j) {
                (void)x86_unmap_page(root, virtual_base + j * PAGE_SIZE, 0);
            }
            return status;
        }
    }
    controller->registers_va = virtual_base;
    controller->registers_size = size;
    controller->registers = (volatile uint8_t *)(uintptr_t)(virtual_base + offset);
    return K_OK;
}

static bool nvme_free_admin_pages(nvme_controller_t *controller) {
    bool submission_released;
    bool completion_released;
    if (controller == 0) return false;
    submission_released = controller->admin_submission_dma.device == 0;
    completion_released = controller->admin_completion_dma.device == 0;
    if (!submission_released) {
        submission_released = dma_unmap_checked(&controller->admin_submission_dma) == K_OK;
    }
    if (!completion_released) {
        completion_released = dma_unmap_checked(&controller->admin_completion_dma) == K_OK;
    }
    if (submission_released) {
        if (controller->admin_submission_page != 0) page_free(controller->admin_submission_page);
        controller->admin_submission_page = 0;
        controller->admin_submission = 0;
    }
    if (completion_released) {
        if (controller->admin_completion_page != 0) page_free(controller->admin_completion_page);
        controller->admin_completion_page = 0;
        controller->admin_completion = 0;
    }
    return submission_released && completion_released;
}

static bool nvme_free_io_queue(nvme_queue_t *queue) {
    bool submission_released;
    bool completion_released;
    if (queue == 0) return false;
    if (queue->pending_ios.next == 0 || queue->pending_ios.prev == 0) {
        list_init(&queue->pending_ios);
        atomic_init(&queue->pending_count, 0U);
        atomic_init(&queue->completion_queued, false);
        atomic_init(&queue->completion_work_refs, 0U);
        atomic_init(&queue->lock.state, 0U);
    }
    /* 调用者必须先停止控制器；这里再撤销队列，确保 DMA 映射最后释放。 */
    queue->active = false;
    nvme_stop_completion_work(queue);
    nvme_abort_queue_pending(queue, K_EDEVREMOVED);
    if (queue->irq_vector != 0 && queue->controller != 0) {
        (void)irq_unregister(queue->irq_vector, nvme_msix_handler,
                              queue->controller);
        queue->irq_vector = 0;
    }
    submission_released = queue->submission_dma.device == 0;
    completion_released = queue->completion_dma.device == 0;
    if (!submission_released) {
        submission_released = dma_unmap_checked(&queue->submission_dma) == K_OK;
    }
    if (!completion_released) {
        completion_released = dma_unmap_checked(&queue->completion_dma) == K_OK;
    }
    if (submission_released) {
        if (queue->submission_page != 0) page_free(queue->submission_page);
        queue->submission_page = 0;
        queue->submission = 0;
    }
    if (completion_released) {
        if (queue->completion_page != 0) page_free(queue->completion_page);
        queue->completion_page = 0;
        queue->completion = 0;
    }
    if (!submission_released || !completion_released) {
        queue->active = false;
        return false;
    }
    queue->queue_id = 0;
    queue->depth = 0;
    queue->submission_tail = 0;
    queue->completion_head = 0;
    queue->phase = 0;
    queue->controller = 0;
    atomic_store_explicit(&queue->pending_count, 0U, memory_order_release);
    list_init(&queue->pending_ios);
    atomic_store_explicit(&queue->completion_queued, false, memory_order_release);
    return true;
}

static kstatus_t nvme_alloc_io_queue(nvme_controller_t *controller,
                                     nvme_queue_t *queue, uint16_t queue_id) {
    if (controller == 0 || queue == 0 || queue_id == 0) return K_EINVAL;
    if (!nvme_free_io_queue(queue)) return K_EBUSY;
    queue->submission_page = page_alloc(0, PAGE_ALLOC_ZERO | PAGE_ALLOC_DMA32);
    queue->completion_page = page_alloc(0, PAGE_ALLOC_ZERO | PAGE_ALLOC_DMA32);
    if (queue->submission_page == 0 || queue->completion_page == 0) {
        nvme_free_io_queue(queue);
        return K_ENOMEM;
    }
    queue->submission_page->owner = PAGE_OWNER_DEVICE;
    queue->completion_page->owner = PAGE_OWNER_DEVICE;
    page_t *submission_pages[1] = {queue->submission_page};
    page_t *completion_pages[1] = {queue->completion_page};
    if (dma_map_pages(controller->device, submission_pages, 1U,
                      DMA_BIDIRECTIONAL, &queue->submission_dma) != K_OK ||
        dma_map_pages(controller->device, completion_pages, 1U,
                      DMA_BIDIRECTIONAL, &queue->completion_dma) != K_OK) {
        nvme_free_io_queue(queue);
        return K_EIO;
    }
    queue->submission = (nvme_command_t *)phys_to_direct(
        page_to_phys(queue->submission_page));
    queue->completion = (nvme_completion_t *)phys_to_direct(
        page_to_phys(queue->completion_page));
    if (queue->submission == 0 || queue->completion == 0) {
        nvme_free_io_queue(queue);
        return K_EIO;
    }
    queue->controller = controller;
    queue->queue_id = queue_id;
    queue->depth = NVME_IO_QUEUE_DEPTH;
    queue->submission_tail = 0;
    queue->completion_head = 0;
    queue->phase = 1U;
    queue->active = false;
    atomic_init(&queue->lock.state, 0U);
    atomic_init(&queue->pending_count, 0U);
    atomic_init(&queue->completion_queued, false);
    atomic_init(&queue->completion_work_refs, 0U);
    list_init(&queue->pending_ios);
    return K_OK;
}

static bool nvme_free_io_queues(nvme_controller_t *controller) {
    bool success = true;
    if (controller == 0) return false;
    for (uint32_t i = 0; i < NVME_MAX_IO_QUEUES; ++i) {
        if (!nvme_free_io_queue(&controller->io_queues[i])) success = false;
    }
    controller->io_queue_count = 0;
    return success;
}

static kstatus_t nvme_alloc_admin_pages(nvme_controller_t *controller) {
    controller->admin_submission_page = page_alloc(0, PAGE_ALLOC_ZERO | PAGE_ALLOC_DMA32);
    controller->admin_completion_page = page_alloc(0, PAGE_ALLOC_ZERO | PAGE_ALLOC_DMA32);
    if (controller->admin_submission_page == 0 || controller->admin_completion_page == 0) {
        nvme_free_admin_pages(controller);
        return K_ENOMEM;
    }
    controller->admin_submission_page->owner = PAGE_OWNER_DEVICE;
    controller->admin_completion_page->owner = PAGE_OWNER_DEVICE;
    page_t *submission_pages[1] = {controller->admin_submission_page};
    page_t *completion_pages[1] = {controller->admin_completion_page};
    if (dma_map_pages(controller->device, submission_pages, 1U,
                      DMA_BIDIRECTIONAL, &controller->admin_submission_dma) != K_OK ||
        dma_map_pages(controller->device, completion_pages, 1U,
                      DMA_BIDIRECTIONAL, &controller->admin_completion_dma) != K_OK) {
        nvme_free_admin_pages(controller);
        return K_EIO;
    }
    controller->admin_submission = (nvme_command_t *)phys_to_direct(
        page_to_phys(controller->admin_submission_page));
    controller->admin_completion = (nvme_completion_t *)phys_to_direct(
        page_to_phys(controller->admin_completion_page));
    if (controller->admin_submission == 0 || controller->admin_completion == 0) {
        nvme_free_admin_pages(controller);
        return K_EIO;
    }
    controller->admin_depth = NVME_ADMIN_QUEUE_DEPTH;
    uint16_t maximum = (uint16_t)((controller->capabilities & 0xFFFFU) + 1U);
    if (controller->admin_depth > maximum) controller->admin_depth = maximum;
    if (controller->admin_depth < 2U) {
        nvme_free_admin_pages(controller);
        return K_EINVAL;
    }
    controller->admin_submission_tail = 0;
    controller->admin_completion_head = 0;
    controller->admin_phase = 1;
    atomic_init(&controller->next_command_id, 1U);
    return K_OK;
}

static uint16_t nvme_next_command_id(nvme_controller_t *controller) {
    unsigned value = atomic_fetch_add_explicit(&controller->next_command_id, 1U,
                                               memory_order_relaxed);
    uint16_t command_id = (uint16_t)value;
    if (command_id == 0) command_id = 1U;
    return command_id;
}

static void nvme_ring_admin_submission(nvme_controller_t *controller) {
    nvme_write32(controller, NVME_REG_DBS, controller->admin_submission_tail);
}

static void nvme_ring_admin_completion(nvme_controller_t *controller) {
    nvme_write32(controller, NVME_REG_DBS + controller->doorbell_stride,
                 controller->admin_completion_head);
}

static kstatus_t nvme_admin_submit(nvme_controller_t *controller,
                                   const nvme_command_t *command,
                                   nvme_completion_t *result) {
    if (controller == 0 || command == 0 || result == 0 || !controller->started) {
        return K_EINVAL;
    }
    nvme_lock(&controller->admin_lock);
    uint16_t slot = controller->admin_submission_tail;
    uint16_t command_id = nvme_next_command_id(controller);
    nvme_command_t submission = *command;
    submission.opcode_flags = (submission.opcode_flags & 0x0000FFFFU) |
                              ((uint32_t)command_id << 16);
    controller->admin_submission[slot] = submission;
    dma_sync_for_device(&controller->admin_submission_dma);
    dma_wmb();
    controller->admin_submission_tail = (uint16_t)((slot + 1U) % controller->admin_depth);
    nvme_ring_admin_submission(controller);

    uint64_t deadline = nvme_timeout_deadline(controller);
    for (;;) {
        dma_sync_for_cpu(&controller->admin_completion_dma);
        nvme_completion_t completion = controller->admin_completion[
            controller->admin_completion_head];
        uint16_t status = completion.status;
        if ((status & 1U) == controller->admin_phase) {
            *result = completion;
            g_nvme_last_completion = status;
            controller->admin_completion_head = (uint16_t)(
                (controller->admin_completion_head + 1U) % controller->admin_depth);
            if (controller->admin_completion_head == 0) controller->admin_phase ^= 1U;
            nvme_ring_admin_completion(controller);
            kstatus_t result_status = ((status >> 1) & 0x7FFFU) == 0 ? K_OK : K_EIO;
            nvme_unlock(&controller->admin_lock);
            return result_status;
        }
        if (nvme_deadline_reached(deadline)) {
            nvme_unlock(&controller->admin_lock);
            return K_ETIMEDOUT;
        }
        __asm__ volatile ("pause");
    }
}

static kstatus_t nvme_identify_controller(nvme_controller_t *controller) {
    page_t *page = page_alloc(0, PAGE_ALLOC_ZERO | PAGE_ALLOC_DMA32);
    if (page == 0) return K_ENOMEM;
    page->owner = PAGE_OWNER_DEVICE;
    page_t *pages[1] = {page};
    dma_mapping_t *mapping = (dma_mapping_t *)kzalloc(sizeof(*mapping), 0);
    if (mapping == 0) {
        page_free(page);
        return K_ENOMEM;
    }
    if (dma_map_pages(controller->device, pages, 1U, DMA_FROM_DEVICE, mapping) != K_OK) {
        kfree(mapping);
        page_free(page);
        return K_EIO;
    }
    nvme_command_t command = {0};
    command.opcode_flags = NVME_ADMIN_IDENTIFY;
    command.prp1 = mapping->segments[0].addr.value;
    command.cdw10 = 1U;
    nvme_completion_t completion = {0};
    kstatus_t status = nvme_admin_submit(controller, &command, &completion);
    if (status == K_OK) {
        const uint8_t *identify = (const uint8_t *)phys_to_direct(page_to_phys(page));
        if (identify == 0) status = K_EIO;
        else {
            controller->namespace_count = *(const uint32_t *)(identify + 516U);
            controller->identified = true;
        }
    }
    dma_sync_for_cpu(mapping);
    if (nvme_release_transient_dma(mapping, page) != K_OK && status == K_OK) {
        status = K_EIO;
    }
    return status;
}

static kstatus_t nvme_identify_namespace(nvme_controller_t *controller) {
    page_t *page;
    page_t *pages[1];
    dma_mapping_t *mapping;
    nvme_command_t command = {0};
    nvme_completion_t completion = {0};
    kstatus_t status;
    const uint8_t *identify;
    uint8_t flbas;
    uint8_t lbads;

    if (controller == 0 || controller->device == 0 ||
        controller->namespace_count == 0U) return K_ENOENT;
    page = page_alloc(0, PAGE_ALLOC_ZERO | PAGE_ALLOC_DMA32);
    if (page == 0) return K_ENOMEM;
    page->owner = PAGE_OWNER_DEVICE;
    pages[0] = page;
    mapping = (dma_mapping_t *)kzalloc(sizeof(*mapping), 0);
    if (mapping == 0) {
        page_free(page);
        return K_ENOMEM;
    }
    status = dma_map_pages(controller->device, pages, 1U, DMA_FROM_DEVICE, mapping);
    if (status != K_OK) {
        kfree(mapping);
        page_free(page);
        return status;
    }

    command.opcode_flags = NVME_ADMIN_IDENTIFY;
    command.namespace_id = 1U;
    command.prp1 = mapping->segments[0].addr.value;
    /* CNS=0：Identify Namespace。 */
    command.cdw10 = 0U;
    status = nvme_admin_submit(controller, &command, &completion);
    if (status == K_OK) {
        identify = (const uint8_t *)phys_to_direct(page_to_phys(page));
        if (identify == 0) {
            status = K_EIO;
        } else {
            controller->namespace_block_count =
                *(const uint64_t *)(const void *)(identify + 0U);
            flbas = identify[26U] & 0x0FU;
            lbads = identify[128U + (uint32_t)flbas * 4U + 2U];
            if (controller->namespace_block_count == 0U || lbads >= 32U) {
                status = K_EIO;
            } else {
                controller->namespace_block_size = 1U << lbads;
            }
        }
    }
    dma_sync_for_cpu(mapping);
    if (nvme_release_transient_dma(mapping, page) != K_OK && status == K_OK) {
        status = K_EIO;
    }
    return status;
}

static uint16_t nvme_desired_io_queue_count(const nvme_controller_t *controller) {
    uint32_t count = 1U;
    const x86_acpi_platform_t *platform = x86_acpi_platform();
    if (platform != 0 && platform->cpu_count > count) count = platform->cpu_count;
    if (count > NVME_MAX_IO_QUEUES) count = NVME_MAX_IO_QUEUES;
    if (controller != 0) {
        paddr_t table;
        uint16_t entries;
        if (pci_msix_table(controller->pci, &table, &entries) == K_OK &&
            entries != 0 && count > entries) count = entries;
    }
    return (uint16_t)count;
}

static kstatus_t nvme_set_io_queue_count(nvme_controller_t *controller,
                                         uint16_t desired, uint16_t *actual) {
    if (controller == 0 || desired == 0 || actual == 0) return K_EINVAL;
    nvme_command_t command = {0};
    command.opcode_flags = NVME_ADMIN_SET_FEATURES;
    command.cdw10 = NVME_FEATURE_NUMBER_QUEUES;
    command.cdw11 = ((uint32_t)(desired - 1U) << 16) | (desired - 1U);
    nvme_completion_t completion = {0};
    kstatus_t status = nvme_admin_submit(controller, &command, &completion);
    if (status != K_OK) return status;
    uint16_t completion_queues = (uint16_t)((completion.result & 0xFFFFU) + 1U);
    uint16_t submission_queues = (uint16_t)(((completion.result >> 16) & 0xFFFFU) + 1U);
    if (completion_queues == 0 || submission_queues == 0) return K_EIO;
    *actual = completion_queues < submission_queues ? completion_queues : submission_queues;
    if (*actual > desired) *actual = desired;
    return K_OK;
}

static void nvme_try_bind_msix(nvme_controller_t *controller,
                               nvme_queue_t *queue, uint16_t queue_index) {
    if (controller == 0 || queue == 0 || controller->pci == 0) return;
    paddr_t table;
    uint16_t entries;
    if (pci_msix_table(controller->pci, &table, &entries) != K_OK ||
        queue_index >= entries) return;
    uint8_t vector = (uint8_t)(0x60U + queue_index);
    if (vector > IRQ_VECTOR_LAST ||
        irq_register(vector, nvme_msix_handler, controller) != K_OK) return;
    if (pci_msix_configure(controller->pci, queue_index,
                           x86_current_apic_id(), vector) != K_OK) {
        (void)irq_unregister(vector, nvme_msix_handler, controller);
        return;
    }
    queue->irq_vector = vector;
}

static bool nvme_rebind_msix(nvme_controller_t *controller,
                             nvme_queue_t *queue, uint16_t msix_entry,
                             uint8_t new_vector) {
    uint8_t old_vector;
    bool restored = false;
    if (controller == 0 || queue == 0 || controller->pci == 0 ||
        queue->irq_vector == 0U || new_vector < IRQ_VECTOR_FIRST ||
        new_vector > IRQ_VECTOR_LAST || new_vector == queue->irq_vector) {
        return new_vector == (queue != 0 ? queue->irq_vector : 0U);
    }
    old_vector = queue->irq_vector;

    /* 先屏蔽设备，再拆除旧路由，避免换绑窗口产生未知中断。 */
    if (pci_msix_mask(controller->pci, msix_entry, true) != K_OK ||
        irq_unregister(old_vector, nvme_msix_handler, controller) != K_OK) {
        (void)pci_msix_mask(controller->pci, msix_entry, false);
        return false;
    }
    if (irq_register(new_vector, nvme_msix_handler, controller) == K_OK &&
        pci_msix_configure(controller->pci, msix_entry, x86_current_apic_id(),
                           new_vector) == K_OK) {
        queue->irq_vector = new_vector;
        return true;
    }

    /* 新路由失败时恢复旧路由；恢复失败则保持无中断状态，由轮询路径兜底。 */
    (void)irq_unregister(new_vector, nvme_msix_handler, controller);
    if (irq_register(old_vector, nvme_msix_handler, controller) == K_OK &&
        pci_msix_configure(controller->pci, msix_entry, x86_current_apic_id(),
                           old_vector) == K_OK) {
        restored = true;
        queue->irq_vector = old_vector;
    } else {
        queue->irq_vector = 0U;
    }
    return restored;
}

static bool nvme_msix_rebind_self_test(const nvme_controller_t *controller) {
    paddr_t table;
    uint16_t entries;
    bool rebound = false;
    if (controller == 0 || controller->pci == 0) return false;
    if (pci_msix_table(controller->pci, &table, &entries) != K_OK) {
        /* 没有 MSI-X 的控制器使用轮询完成路径，不需要换绑中断。 */
        return true;
    }
    (void)table;
    for (uint16_t index = 0; index < controller->io_queue_count; ++index) {
        nvme_queue_t *queue = (nvme_queue_t *)&controller->io_queues[index];
        uint8_t old_vector = queue->irq_vector;
        uint8_t alternate = (uint8_t)(0xA0U + index);
        if (old_vector == 0U || index >= entries || alternate > IRQ_VECTOR_LAST) {
            continue;
        }
        rebound = true;
        if (!nvme_rebind_msix((nvme_controller_t *)controller, queue, index,
                              alternate) ||
            queue->irq_vector != alternate ||
            !nvme_rebind_msix((nvme_controller_t *)controller, queue, index,
                              old_vector) ||
            queue->irq_vector != old_vector) {
            return false;
        }
    }
    return rebound;
}

static kstatus_t nvme_create_io_queues(nvme_controller_t *controller,
                                       uint16_t queue_count) {
    if (controller == 0 || controller->namespace_count == 0) return K_ENOENT;
    if (queue_count == 0 || queue_count > NVME_MAX_IO_QUEUES) return K_EINVAL;
    if (!nvme_free_io_queues(controller)) return K_EBUSY;

    for (uint16_t index = 0; index < queue_count; ++index) {
        nvme_queue_t *queue = &controller->io_queues[index];
        kstatus_t status = nvme_alloc_io_queue(controller, queue,
                                                (uint16_t)(index + 1U));
        if (status != K_OK) {
            if (!nvme_free_io_queues(controller)) return K_EBUSY;
            return status;
        }
        nvme_try_bind_msix(controller, queue, index);

        nvme_command_t command = {0};
        command.opcode_flags = NVME_ADMIN_CREATE_CQ;
        command.prp1 = queue->completion_dma.segments[0].addr.value;
        /* CDW10：低 16 位为 CQID，高 16 位为 QSIZE。 */
        command.cdw10 = ((uint32_t)(queue->depth - 1U) << 16) |
                        queue->queue_id;
        uint32_t cq_flags = queue->irq_vector != 0 ? 3U : 1U;
        /* CQ 命令的高 16 位是 MSI-X 表项索引，不是 IDT 向量号。 */
        command.cdw11 = ((uint32_t)index << 16) | cq_flags;
        nvme_completion_t completion = {0};
        g_nvme_last_stage = 710U + index * 2U;
        status = nvme_admin_submit(controller, &command, &completion);
        if (status != K_OK) {
            if (!nvme_free_io_queues(controller)) return K_EBUSY;
            return status;
        }

        command = (nvme_command_t){0};
        command.opcode_flags = NVME_ADMIN_CREATE_SQ;
        command.prp1 = queue->submission_dma.segments[0].addr.value;
        /* CDW10：低 16 位为 SQID，高 16 位为 QSIZE。 */
        command.cdw10 = ((uint32_t)(queue->depth - 1U) << 16) |
                        queue->queue_id;
        command.cdw11 = ((uint32_t)queue->queue_id << 16) | 1U;
        completion = (nvme_completion_t){0};
        g_nvme_last_stage = 711U + index * 2U;
        status = nvme_admin_submit(controller, &command, &completion);
        if (status != K_OK) {
            nvme_free_io_queues(controller);
            return status;
        }
        queue->active = true;
        ++controller->io_queue_count;
    }
    atomic_store_explicit(&controller->next_io_queue, 0U, memory_order_release);
    return K_OK;
}

#if 0
static kstatus_t nvme_create_io_queue(nvme_controller_t *controller) {
    if (controller == 0 || controller->namespace_count == 0) return K_ENOENT;
    nvme_queue_t *queue = &controller->io_queue;
    kstatus_t status = nvme_alloc_io_queue(controller);
    if (status != K_OK) return status;

    nvme_command_t command = {0};
    command.opcode_flags = NVME_ADMIN_CREATE_CQ;
    command.prp1 = page_to_phys(queue->completion_page).value;
    /* CDW10：低 16 位为 CQID，高 16 位为 QSIZE。 */
    command.cdw10 = ((uint32_t)(queue->depth - 1U) << 16) |
                    queue->queue_id;
    command.cdw11 = 1U; /* 物理连续队列；暂时使用轮询完成，不启用中断。 */
    nvme_completion_t completion = {0};
    g_nvme_last_stage = 71U;
    status = nvme_admin_submit(controller, &command, &completion);
    if (status != K_OK) {
        nvme_free_io_queue(queue);
        return status;
    }

    command = (nvme_command_t){0};
    command.opcode_flags = NVME_ADMIN_CREATE_SQ;
    command.prp1 = page_to_phys(queue->submission_page).value;
    /* CDW10：低 16 位为 SQID，高 16 位为 QSIZE。 */
    command.cdw10 = ((uint32_t)(queue->depth - 1U) << 16) |
                    queue->queue_id;
    command.cdw11 = ((uint32_t)queue->queue_id << 16) | 1U;
    completion = (nvme_completion_t){0};
    g_nvme_last_stage = 72U;
    status = nvme_admin_submit(controller, &command, &completion);
    if (status != K_OK) {
        nvme_free_io_queue(queue);
        return status;
    }
    queue->active = true;
    return K_OK;
}
#endif

static kstatus_t nvme_stop(nvme_controller_t *controller) {
    if (controller == 0 || controller->registers == 0) return K_EINVAL;
    if ((nvme_read32(controller, NVME_REG_CC) & NVME_CC_ENABLE) == 0) return K_OK;
    nvme_write32(controller, NVME_REG_CC, 0);
    return nvme_wait_ready(controller, false);
}

static kstatus_t nvme_start(device_t *device) {
    g_nvme_last_stage = 1U;
    if (device == 0 || device->driver_data == 0) return nvme_record_status(K_EINVAL);
    nvme_controller_t *controller = (nvme_controller_t *)device->driver_data;
    g_nvme_last_stage = 2U;
    kstatus_t status = pci_enable_memory_busmaster(controller->pci);
    if (status != K_OK) return nvme_record_status(status);
    controller->capabilities = nvme_read64(controller, NVME_REG_CAP);
    controller->doorbell_stride = 4U << ((controller->capabilities >> 32) & 0xFU);
    if (controller->doorbell_stride == 0 || controller->doorbell_stride > 4096U) {
        return nvme_record_status(K_EINVAL);
    }
    g_nvme_last_stage = 3U;
    status = nvme_stop(controller);
    if (status != K_OK) return nvme_record_status(status);
    bool queues_released = nvme_free_io_queues(controller);
    bool admin_released = nvme_free_admin_pages(controller);
    if (!queues_released || !admin_released) {
        return nvme_record_status(K_EBUSY);
    }
    g_nvme_last_stage = 4U;
    status = nvme_alloc_admin_pages(controller);
    if (status != K_OK) return nvme_record_status(status);
    nvme_write32(controller, NVME_REG_AQA,
                 ((uint32_t)(controller->admin_depth - 1U) << 16) |
                 (controller->admin_depth - 1U));
    nvme_write64(controller, NVME_REG_ASQ,
                 controller->admin_submission_dma.segments[0].addr.value);
    nvme_write64(controller, NVME_REG_ACQ,
                 controller->admin_completion_dma.segments[0].addr.value);
    /* IOSQES=6、IOCQES=4，MPS=0，CSS=0。 */
    nvme_write32(controller, NVME_REG_CC, (6U << 16) | (4U << 20) | NVME_CC_ENABLE);
    g_nvme_last_stage = 5U;
    status = nvme_wait_ready(controller, true);
    if (status != K_OK) return nvme_record_status(status);
    controller->started = true;
    g_nvme_last_stage = 6U;
    status = nvme_identify_controller(controller);
    if (status != K_OK) {
        controller->started = false;
        (void)nvme_stop(controller);
        if (!nvme_free_io_queues(controller) ||
            !nvme_free_admin_pages(controller)) status = K_EBUSY;
        return nvme_record_status(status);
    }
    if (controller->namespace_count != 0U) {
        status = nvme_identify_namespace(controller);
        if (status != K_OK) {
            controller->started = false;
            (void)nvme_stop(controller);
            if (!nvme_free_io_queues(controller) ||
                !nvme_free_admin_pages(controller)) status = K_EBUSY;
            return nvme_record_status(status);
        }
    }
    if (controller->namespace_count != 0) {
        g_nvme_last_stage = 7U;
        uint16_t desired = nvme_desired_io_queue_count(controller);
        uint16_t actual = 0;
        status = nvme_set_io_queue_count(controller, desired, &actual);
        if (status == K_OK) status = nvme_create_io_queues(controller, actual);
        if (status != K_OK) {
            controller->started = false;
            (void)nvme_stop(controller);
            if (!nvme_free_io_queues(controller) ||
                !nvme_free_admin_pages(controller)) status = K_EBUSY;
            return nvme_record_status(status);
        }
    }
    g_nvme_last_status = K_OK;
    return K_OK;
}

static bool nvme_stop_device_checked(device_t *device) {
    if (device == 0 || device->driver_data == 0) return false;
    nvme_controller_t *controller = (nvme_controller_t *)device->driver_data;
    controller->started = false;
    kstatus_t stop_status = nvme_stop(controller);
    bool queues_released = nvme_free_io_queues(controller);
    bool admin_released = nvme_free_admin_pages(controller);
    return stop_status == K_OK && queues_released && admin_released;
}

static void nvme_stop_device(device_t *device) {
    (void)nvme_stop_device_checked(device);
}

kstatus_t nvme_recover_after_timeout(device_t *device) {
    if (device == 0 || device->driver_data == 0) return K_EINVAL;

    nvme_controller_t *controller =
        (nvme_controller_t *)device->driver_data;

    /*
     * Ignore late MSI-X while nvme_start() disables the controller and tears
     * down the old queues. nvme_free_io_queues() aborts pending mappings only
     * after the controller is no longer allowed to DMA.
     */
    controller->started = false;

    kstatus_t status = nvme_start(device);
    g_nvme_last_status = status;
    return status;
}

static void nvme_ring_io_submission(nvme_controller_t *controller,
                                    const nvme_queue_t *queue) {
    uint32_t offset = NVME_REG_DBS +
                      (uint32_t)queue->queue_id * 2U * controller->doorbell_stride;
    nvme_write32(controller, offset, queue->submission_tail);
}

static void nvme_ring_io_completion(nvme_controller_t *controller,
                                    const nvme_queue_t *queue) {
    uint32_t offset = NVME_REG_DBS +
                      ((uint32_t)queue->queue_id * 2U + 1U) *
                          controller->doorbell_stride;
    nvme_write32(controller, offset, queue->completion_head);
}

static nvme_pending_io_t *nvme_pending_for_id_locked(nvme_queue_t *queue,
                                                       uint16_t command_id) {
    if (queue == 0) return 0;
    for (list_head_t *node = queue->pending_ios.next;
         node != &queue->pending_ios; node = node->next) {
        nvme_pending_io_t *pending = (nvme_pending_io_t *)((uint8_t *)node -
            __builtin_offsetof(nvme_pending_io_t, node));
        if (pending->command_id == command_id) return pending;
    }
    return 0;
}

static void nvme_pending_unlink_locked(nvme_queue_t *queue,
                                        nvme_pending_io_t *pending) {
    if (queue == 0 || pending == 0) return;
    if (pending->node.next != &pending->node && pending->node.prev != &pending->node) {
        pending->node.prev->next = pending->node.next;
        pending->node.next->prev = pending->node.prev;
        list_init(&pending->node);
        atomic_fetch_sub_explicit(&queue->pending_count, 1U, memory_order_release);
    }
}

static void nvme_pending_release(nvme_pending_io_t *pending,
                                 kstatus_t completion_status,
                                 uint64_t completion_bytes) {
    io_request_t *request;
    if (pending == 0) return;
    request = pending->request;
    if (pending->mapping != 0) {
        /* CQ 已经确认设备不再访问缓冲区，此时才允许同步和解除映射。 */
        dma_sync_for_cpu(pending->mapping);
        (void)nvme_release_transient_dma(pending->mapping, 0);
        pending->mapping = 0;
    }
    if (request != 0 &&
        atomic_load_explicit(&request->state, memory_order_acquire) != IOREQ_CANCELLED) {
        io_complete(request, completion_status, completion_bytes);
    }
    if (request != 0) object_put(request);
    kfree(pending);
}

static void nvme_deferred_complete(void *argument) {
    nvme_queue_t *queue = (nvme_queue_t *)argument;
    nvme_controller_t *controller;
    uint32_t processed = 0U;
    if (queue == 0) return;
    controller = queue->controller;
    if (controller == 0) {
        atomic_store_explicit(&queue->completion_queued, false,
                              memory_order_release);
        nvme_completion_ref_put(queue);
        return;
    }

    nvme_lock(&queue->lock);
    if (queue->completion != 0) dma_sync_for_cpu(&queue->completion_dma);
    while (processed < 32U && queue->completion != 0) {
        nvme_completion_t completion = queue->completion[queue->completion_head];
        if ((completion.status & 1U) != queue->phase) break;
        queue->completion_head = (uint16_t)((queue->completion_head + 1U) %
                                            queue->depth);
        if (queue->completion_head == 0U) queue->phase ^= 1U;
        nvme_ring_io_completion(controller, queue);
        nvme_pending_io_t *pending = nvme_pending_for_id_locked(
            queue, completion.command_id);
        if (pending != 0) nvme_pending_unlink_locked(queue, pending);
        nvme_unlock(&queue->lock);
        if (pending != 0) {
            kstatus_t status = ((completion.status >> 1) & 0x7FFFU) == 0U ?
                                K_OK : K_EIO;
            g_nvme_last_completion = completion.status;
            nvme_pending_release(pending, status,
                                 status == K_OK ? pending->bytes : 0U);
        }
        ++processed;
        nvme_lock(&queue->lock);
        if (queue->completion != 0) dma_sync_for_cpu(&queue->completion_dma);
    }
    nvme_unlock(&queue->lock);
    /* 用原子标志发布本轮 CQ 消费结果，避免轮询线程漏掉新完成项。 */
    atomic_store_explicit(&queue->completion_queued, false, memory_order_release);
    nvme_completion_ref_put(queue);

    /* 队列深度超过一次 deferred 预算时，继续排队，避免丢失 CQ。 */
    if (processed == 32U && queue->active) (void)nvme_schedule_queue_completion(queue);
}

static void nvme_cancel_request(io_request_t *request) {
    device_t *device;
    nvme_controller_t *controller;
    if (request == 0 || request->device == 0) return;
    device = request->device;
    controller = (nvme_controller_t *)device->driver_data;
    if (controller == 0) return;
    for (uint16_t index = 0U; index < controller->io_queue_count; ++index) {
        nvme_queue_t *queue = &controller->io_queues[index];
        nvme_lock(&queue->lock);
        for (list_head_t *node = queue->pending_ios.next;
             node != &queue->pending_ios; node = node->next) {
            nvme_pending_io_t *pending = (nvme_pending_io_t *)((uint8_t *)node -
                __builtin_offsetof(nvme_pending_io_t, node));
            if (pending->request == request) {
                pending->cancelled = true;
                nvme_unlock(&queue->lock);
                return;
            }
        }
        nvme_unlock(&queue->lock);
    }
}

static void nvme_abort_queue_pending(nvme_queue_t *queue, kstatus_t status) {
    if (queue == 0) return;
    for (;;) {
        nvme_pending_io_t *pending = 0;
        nvme_lock(&queue->lock);
        if (!list_empty(&queue->pending_ios)) {
            list_head_t *node = queue->pending_ios.next;
            pending = (nvme_pending_io_t *)((uint8_t *)node -
                __builtin_offsetof(nvme_pending_io_t, node));
            nvme_pending_unlink_locked(queue, pending);
        }
        nvme_unlock(&queue->lock);
        if (pending == 0) return;
        pending->cancelled = true;
        nvme_pending_release(pending, status, 0U);
    }
}

#if 0
static kstatus_t nvme_submit_io(device_t *device, io_request_t *request) {
    /* 管理队列已经可用；I/O 队列和 BIO 映射在下一层接入前明确拒绝请求。 */
    if (device == 0 || request == 0 || device->driver_data == 0) return K_EINVAL;
    nvme_controller_t *controller = (nvme_controller_t *)device->driver_data;
    if (!controller->started || !controller->identified ||
        controller->io_queue_count == 0) return K_EIO;
    uint32_t queue_index;
    if ((request->flags & IOREQ_BLOCK_QUEUE_VALID) != 0) {
        queue_index = (request->flags >> IOREQ_BLOCK_QUEUE_SHIFT) &
                      IOREQ_BLOCK_QUEUE_MASK;
        queue_index %= controller->io_queue_count;
    } else {
        queue_index = atomic_fetch_add_explicit(&controller->next_io_queue, 1U,
                                                memory_order_relaxed) %
                       controller->io_queue_count;
    }
    nvme_queue_t *queue = &controller->io_queues[queue_index];

    nvme_command_t command = {0};
    command.namespace_id = 1U;
    uint64_t bytes = 0;
    dma_mapping_t *mapping = 0;
    bio_t *bio = (bio_t *)request->completion_target;

    if (request->opcode == IO_FLUSH) {
        command.opcode_flags = NVME_IO_FLUSH;
    } else {
        if (bio == 0 || bio->io != request || bio->vec_count != 1U ||
            bio->vecs == 0 ||
            (bio->op != BIO_OP_READ && bio->op != BIO_OP_WRITE)) return K_EINVAL;
        const bio_vec_t *vector = &bio->vecs[0];
        if (vector->page == 0 || vector->length == 0 ||
            vector->offset >= PAGE_SIZE || vector->length > PAGE_SIZE - vector->offset ||
            (vector->length & 511U) != 0U) return K_EINVAL;
        bytes = vector->length;
        uint64_t sectors = bytes / 512U;
        if (sectors == 0 || sectors > 0x10000ULL) return K_EINVAL;
        page_t *pages[1] = {vector->page};
        enum dma_direction direction = bio->op == BIO_OP_READ ?
                                        DMA_FROM_DEVICE : DMA_TO_DEVICE;
        mapping = (dma_mapping_t *)kzalloc(sizeof(*mapping), 0);
        if (mapping == 0) return K_ENOMEM;
        kstatus_t status = dma_map_pages(device, pages, 1U, direction, mapping);
        if (status != K_OK) {
            kfree(mapping);
            return status;
        }
        iova_t address = iova_make(mapping->segments[0].addr.value + vector->offset);
        enum dma_device_access access = bio->op == BIO_OP_READ ?
                                         DMA_DEVICE_WRITE : DMA_DEVICE_READ;
        if (mapping->segment_count != 1U ||
            dma_validate_access(device, address, bytes, access) != K_OK) {
            kstatus_t cleanup_status = nvme_release_transient_dma(mapping, 0);
            mapping = 0;
            return cleanup_status == K_OK ? K_EACCES : K_EIO;
        }
        dma_sync_for_device(mapping);
        command.opcode_flags = bio->op == BIO_OP_READ ? NVME_IO_READ : NVME_IO_WRITE;
        command.prp1 = address.value;
        command.cdw10 = (uint32_t)bio->lba;
        command.cdw11 = (uint32_t)(bio->lba >> 32);
        command.cdw12 = (uint32_t)(sectors - 1U);
    }

    nvme_completion_t completion = {0};
    kstatus_t status = nvme_io_submit_command(controller, queue, &command,
                                               &completion);
    if (mapping != 0) {
        dma_sync_for_cpu(mapping);
        kstatus_t cleanup_status = nvme_release_transient_dma(mapping, 0);
        if (cleanup_status != K_OK && status == K_OK) status = K_EIO;
    }
    io_complete(request, status, status == K_OK ? bytes : 0U);
    return status;
}

#endif

static void nvme_publish_pending_locked(nvme_queue_t *queue,
                                        nvme_pending_io_t *pending,
                                        const nvme_command_t *command) {
    uint16_t slot;
    nvme_command_t submission;
    if (queue == 0 || pending == 0 || command == 0) return;
    slot = queue->submission_tail;
    submission = *command;
    submission.opcode_flags = (submission.opcode_flags & 0x0000FFFFU) |
                              ((uint32_t)pending->command_id << 16);
    queue->submission[slot] = submission;
    dma_sync_for_device(&queue->submission_dma);
    dma_wmb();
    queue->submission_tail = (uint16_t)((slot + 1U) % queue->depth);
    pending->node.next = &queue->pending_ios;
    pending->node.prev = queue->pending_ios.prev;
    queue->pending_ios.prev->next = &pending->node;
    queue->pending_ios.prev = &pending->node;
    atomic_fetch_add_explicit(&queue->pending_count, 1U, memory_order_release);
    nvme_ring_io_submission(queue->controller, queue);
}

static uint16_t nvme_allocate_io_command_id_locked(nvme_controller_t *controller,
                                                   nvme_queue_t *queue) {
    if (controller == 0 || queue == 0) return 0U;
    for (uint32_t attempt = 0U; attempt < 0xFFFFU; ++attempt) {
        uint16_t candidate = nvme_next_command_id(controller);
        if (candidate != 0U && nvme_pending_for_id_locked(queue, candidate) == 0) {
            return candidate;
        }
    }
    return 0U;
}

static kstatus_t nvme_prepare_request_dma(device_t *device,
                                          io_request_t *request,
                                          nvme_command_t *command,
                                          dma_mapping_t **mapping_out,
                                          uint64_t *bytes_out) {
    bio_t *bio;
    const bio_vec_t *bio_vector = 0;
    const io_vec_t *io_vector;
    page_t *page;
    uint64_t physical = 0U;
    uint64_t end_physical;
    uint64_t bytes;
    uint64_t lba;
    uint64_t sectors;
    uint64_t page_offset = 0U;
    enum dma_direction direction;
    enum dma_device_access access;
    dma_mapping_t *mapping;
    page_t *pages[1];
    kstatus_t status;

    if (device == 0 || request == 0 || command == 0 || mapping_out == 0 ||
        bytes_out == 0) return K_EINVAL;
    *mapping_out = 0;
    *bytes_out = 0U;
    command->namespace_id = 1U;
    if (request->opcode == IO_FLUSH) {
        command->opcode_flags = NVME_IO_FLUSH;
        return K_OK;
    }
    if (request->opcode != IO_READ && request->opcode != IO_WRITE) return K_EINVAL;

    bio = (bio_t *)request->completion_target;
    if (bio != 0) {
        if (bio->io != request || bio->vec_count != 1U || bio->vecs == 0 ||
            (bio->op != BIO_OP_READ && bio->op != BIO_OP_WRITE)) return K_EINVAL;
        bio_vector = &bio->vecs[0];
        if (bio_vector->page == 0 || bio_vector->length == 0U ||
            bio_vector->offset >= PAGE_SIZE ||
            bio_vector->length > PAGE_SIZE - bio_vector->offset ||
            (bio_vector->length & 511U) != 0U) return K_EINVAL;
        bytes = bio_vector->length;
        lba = bio->lba;
        page = bio_vector->page;
        page_offset = bio_vector->offset;
        direction = bio->op == BIO_OP_READ ? DMA_FROM_DEVICE : DMA_TO_DEVICE;
    } else {
        if (request->vec_count != 1U || request->vecs == 0 ||
            request->vecs[0].base == 0 || request->vecs[0].length == 0U) {
            return K_EINVAL;
        }
        io_vector = &request->vecs[0];
        bytes = io_vector->length;
        if (bytes > PAGE_SIZE || (bytes & 511U) != 0U) return K_EINVAL;
        physical = direct_to_phys(io_vector->base).value;
        if (physical == UINT64_MAX || physical > UINT64_MAX - (bytes - 1U)) {
            return K_EACCES;
        }
        end_physical = physical + bytes - 1U;
        if ((physical >> PAGE_SHIFT) != (end_physical >> PAGE_SHIFT)) return K_EINVAL;
        page = phys_to_page(paddr_make(physical & ~(PAGE_SIZE - 1ULL)));
        if (page == 0) return K_EACCES;
        page_offset = physical & (PAGE_SIZE - 1ULL);
        lba = request->offset / 512U;
        direction = request->opcode == IO_READ ? DMA_FROM_DEVICE : DMA_TO_DEVICE;
    }
    if ((request->offset & 511U) != 0U && bio == 0) return K_EINVAL;
    sectors = bytes / 512U;
    if (sectors == 0U || sectors > 0x10000ULL || lba > UINT64_MAX - sectors) {
        return K_EINVAL;
    }
    pages[0] = page;
    mapping = (dma_mapping_t *)kzalloc(sizeof(*mapping), 0);
    if (mapping == 0) return K_ENOMEM;
    status = dma_map_pages(device, pages, 1U, direction, mapping);
    if (status != K_OK) {
        kfree(mapping);
        return status;
    }
    iova_t address = iova_make(mapping->segments[0].addr.value + page_offset);
    access = request->opcode == IO_READ ? DMA_DEVICE_WRITE : DMA_DEVICE_READ;
    if (mapping->segment_count != 1U ||
        dma_validate_access(device, address, bytes, access) != K_OK) {
        status = nvme_release_transient_dma(mapping, 0);
        return status == K_OK ? K_EACCES : K_EIO;
    }
    dma_sync_for_device(mapping);
    command->opcode_flags = request->opcode == IO_READ ? NVME_IO_READ : NVME_IO_WRITE;
    command->prp1 = address.value;
    command->cdw10 = (uint32_t)lba;
    command->cdw11 = (uint32_t)(lba >> 32);
    command->cdw12 = (uint32_t)(sectors - 1U);
    *mapping_out = mapping;
    *bytes_out = bytes;
    return K_OK;
}

static kstatus_t nvme_submit_io(device_t *device, io_request_t *request) {
    nvme_controller_t *controller;
    nvme_queue_t *queue;
    nvme_pending_io_t *pending;
    nvme_command_t command = {0};
    dma_mapping_t *mapping = 0;
    uint64_t bytes = 0U;
    kstatus_t status;
    uint32_t queue_index;

    if (device == 0 || request == 0 || device->driver_data == 0) return K_EINVAL;
    request->cancel = nvme_cancel_request;
    controller = (nvme_controller_t *)device->driver_data;
    if (!controller->started || !controller->identified ||
        controller->io_queue_count == 0U) return K_EIO;
    if ((request->flags & IOREQ_BLOCK_QUEUE_VALID) != 0U) {
        queue_index = (request->flags >> IOREQ_BLOCK_QUEUE_SHIFT) &
                      IOREQ_BLOCK_QUEUE_MASK;
        queue_index %= controller->io_queue_count;
    } else {
        queue_index = atomic_fetch_add_explicit(&controller->next_io_queue, 1U,
                                                memory_order_relaxed) %
                       controller->io_queue_count;
    }
    queue = &controller->io_queues[queue_index];
    status = nvme_prepare_request_dma(device, request, &command, &mapping, &bytes);
    if (status != K_OK) return status;
    pending = (nvme_pending_io_t *)kzalloc(sizeof(*pending), 0);
    if (pending == 0) {
        (void)nvme_release_transient_dma(mapping, 0);
        return K_ENOMEM;
    }
    list_init(&pending->node);
    pending->queue = queue;
    pending->request = request;
    pending->mapping = mapping;
    pending->bytes = bytes;
    object_get(request);

    nvme_lock(&queue->lock);
    if (!queue->active || queue->controller != controller ||
        atomic_load_explicit(&request->state, memory_order_acquire) != IOREQ_SUBMITTED ||
        atomic_load_explicit(&queue->pending_count, memory_order_acquire) >=
            (unsigned)(queue->depth - 1U)) {
        nvme_unlock(&queue->lock);
        object_put(request);
        kfree(pending);
        (void)nvme_release_transient_dma(mapping, 0);
        return K_EBUSY;
    }
    pending->command_id = nvme_allocate_io_command_id_locked(controller, queue);
    if (pending->command_id == 0U) {
        nvme_unlock(&queue->lock);
        object_put(request);
        kfree(pending);
        (void)nvme_release_transient_dma(mapping, 0);
        return K_EBUSY;
    }
    nvme_publish_pending_locked(queue, pending, &command);
    nvme_unlock(&queue->lock);
    return K_OK;
}

bool nvme_schedule_deferred_poll(void) {
    bool scheduled = false;
    for (uint32_t controller_index = 0U;
         controller_index < NVME_MAX_CONTROLLERS; ++controller_index) {
        nvme_controller_t *controller = g_nvme_controllers[controller_index];
        if (controller == 0 || !controller->started) continue;
        for (uint16_t queue_index = 0U;
             queue_index < controller->io_queue_count; ++queue_index) {
            nvme_queue_t *queue = &controller->io_queues[queue_index];
            if (queue->active &&
                atomic_load_explicit(&queue->pending_count, memory_order_acquire) != 0U &&
                nvme_schedule_queue_completion(queue)) {
                scheduled = true;
            }
        }
    }
    return scheduled;
}

static bool nvme_read_lba0_self_test(const nvme_controller_t *controller,
                                     uint32_t forced_queue) {
    if (controller->device == 0 || controller->namespace_count == 0) return false;
    if (forced_queue != UINT32_MAX && forced_queue >= controller->io_queue_count) {
        return false;
    }

    page_t *page = page_alloc(0, PAGE_ALLOC_ZERO | PAGE_ALLOC_DMA32);
    if (page == 0) return false;
    void *buffer = phys_to_direct(page_to_phys(page));
    if (buffer == 0) {
        page_free(page);
        return false;
    }

    io_vec_t io_vector = { .base = buffer, .length = 512U };
    io_request_t request;
    io_request_init(&request, IO_READ, controller->device, 0, &io_vector, 1U);
    bio_vec_t bio_vector = { .page = page, .offset = 0U, .length = 512U };
    bio_t bio = {0};
    bio.lba = 0;
    bio.op = BIO_OP_READ;
    bio.vecs = &bio_vector;
    bio.vec_count = 1U;
    bio.io = &request;

    request.completion_target = &bio;
    if (forced_queue != UINT32_MAX) {
        request.flags = IOREQ_BLOCK_QUEUE_VALID |
                        ((forced_queue & IOREQ_BLOCK_QUEUE_MASK) <<
                         IOREQ_BLOCK_QUEUE_SHIFT);
    }
    kstatus_t status = io_submit(&request);
    bool page_safe = true;
    g_nvme_last_stage = 900U;
    if (status == K_OK) {
        uint64_t deadline = nvme_timeout_deadline(controller);
        while (atomic_load_explicit(&request.state, memory_order_acquire) ==
                   IOREQ_SUBMITTED && !nvme_deadline_reached(deadline)) {
            /* 启动阶段可能还没有可调度线程，直接消费有限 deferred 配额。 */
            (void)nvme_schedule_deferred_poll();
            (void)deferred_run(8U);
            __asm__ volatile ("pause");
        }
        if (atomic_load_explicit(&request.state, memory_order_acquire) ==
            IOREQ_SUBMITTED) {
            status = K_ETIMEDOUT;
            g_nvme_last_stage = 901U;
        } else {
            status = request.status;
            g_nvme_last_stage = 902U +
                atomic_load_explicit(&request.state, memory_order_acquire);
        }
    }
    g_nvme_last_status = status;
    if (status != K_OK &&
        atomic_load_explicit(&request.state, memory_order_acquire) == IOREQ_SUBMITTED) {
        (void)io_cancel(&request);
    }
    if (status == K_ETIMEDOUT) {
        /* 超时后设备仍可能持有 PRP，必须先停控制器再释放测试页。 */
        page_safe = nvme_stop_device_checked(controller->device);
    }
    bool success = status == K_OK && request.status == K_OK &&
                   request.bytes_done == 512U &&
                   atomic_load_explicit(&request.state, memory_order_acquire) ==
                       IOREQ_COMPLETED;
    if (page_safe) page_free(page);
    return success;
}

bool nvme_hardware_io_self_test(void) {
    const nvme_controller_t *controller = nvme_active_controller();
    if (controller == 0) return !g_nvme_hardware_seen;
    return nvme_read_lba0_self_test(controller, UINT32_MAX);
}

bool nvme_hardware_reset_self_test(void) {
    const nvme_controller_t *before = nvme_active_controller();
    if (before == 0) return !g_nvme_hardware_seen;
    /* 连续提交超过队列深度，验证 SQ/CQ 头尾指针和 phase 的环回。 */
    if (before->io_queue_count == 0U || before->io_queue_count > NVME_MAX_IO_QUEUES) {
        return false;
    }
    for (uint16_t queue = 0; queue < before->io_queue_count; ++queue) {
        const nvme_queue_t *state = &before->io_queues[queue];
        if (!state->active || state->queue_id != queue + 1U ||
            state->depth < 2U || state->depth > NVME_IO_QUEUE_DEPTH) {
            return false;
        }
        for (uint32_t i = 0; i <= state->depth; ++i) {
            if (!nvme_read_lba0_self_test(before, queue)) return false;
        }
    }
    if (before->device == 0 || device_reset(before->device, 1U) != K_OK) {
        return false;
    }

    /* 复位必须保留同一个设备对象，并恢复管理队列、命名空间和 I/O 队列。 */
    const nvme_controller_t *after = nvme_active_controller();
    if (after != before || !after->started || !after->identified ||
        after->namespace_count == 0 || after->io_queue_count == 0) {
        return false;
    }
    if (after->io_queue_count != before->io_queue_count) return false;
    if (!nvme_msix_rebind_self_test(after)) return false;
    for (uint16_t queue = 0; queue < after->io_queue_count; ++queue) {
        const nvme_queue_t *state = &after->io_queues[queue];
        if (!state->active || state->queue_id != queue + 1U ||
            state->depth < 2U || state->depth > NVME_IO_QUEUE_DEPTH) {
            return false;
        }
        for (uint32_t i = 0; i <= state->depth; ++i) {
            if (!nvme_read_lba0_self_test(after, queue)) return false;
        }
    }
    return nvme_read_lba0_self_test(after, UINT32_MAX);
}

static kstatus_t nvme_reset(device_t *device, uint32_t level) {
    (void)level;
    if (device == 0 || device->driver_data == 0) return K_EINVAL;
    nvme_controller_t *controller = (nvme_controller_t *)device->driver_data;
    controller->started = false;
    return nvme_start(device);
}

static const device_ops_t g_nvme_device_ops = {
    .start = nvme_start,
    .stop = nvme_stop_device,
    .submit_io = nvme_submit_io,
    .reset = nvme_reset,
    .set_power = 0,
};

static kstatus_t nvme_probe(device_t *device) {
    if (device == 0 || device->bus_data == 0) return K_EINVAL;
    pci_device_t *pci = (pci_device_t *)device->bus_data;
    if (pci->class_code != NVME_CLASS_MASS_STORAGE ||
        pci->subclass != NVME_SUBCLASS_NVM || pci->prog_if != NVME_PROGIF_NVM) {
        return K_ENOENT;
    }
    g_nvme_hardware_seen = true;
    nvme_controller_t *controller = (nvme_controller_t *)kzalloc(sizeof(*controller), 0);
    if (controller == 0) return K_ENOMEM;
    controller->device = device;
    controller->pci = pci;
    atomic_init(&controller->admin_lock.state, 0U);
    atomic_init(&controller->next_io_queue, 0U);
    kstatus_t status = nvme_map_registers(controller);
    if (status != K_OK) {
        g_nvme_last_status = status;
        kfree(controller);
        return status;
    }
    device->driver_data = controller;
    device->ops = &g_nvme_device_ops;
    for (uint32_t i = 0; i < NVME_MAX_CONTROLLERS; ++i) {
        if (g_nvme_controllers[i] == 0) {
            g_nvme_controllers[i] = controller;
            return K_OK;
        }
    }
    device->driver_data = 0;
    device->ops = 0;
    nvme_unmap_registers(controller);
    kfree(controller);
    g_nvme_last_status = K_ENOMEM;
    return K_ENOMEM;
}

static void nvme_remove(device_t *device) {
    if (device == 0 || device->driver_data == 0) return;
    nvme_controller_t *controller = (nvme_controller_t *)device->driver_data;
    if (!nvme_stop_device_checked(device)) return;
    for (uint32_t i = 0; i < NVME_MAX_CONTROLLERS; ++i) {
        if (g_nvme_controllers[i] == controller) g_nvme_controllers[i] = 0;
    }
    nvme_unmap_registers(controller);
    device->driver_data = 0;
    device->ops = 0;
    kfree(controller);
}

kstatus_t nvme_driver_register(void) {
    unsigned expected = 0U;
    if (!atomic_compare_exchange_strong_explicit(&g_nvme_driver_state, &expected, 1U,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        while (atomic_load_explicit(&g_nvme_driver_state, memory_order_acquire) == 1U) {
            __asm__ volatile ("pause");
        }
        return K_OK;
    }
    driver_object_init(&g_nvme_driver, "nvme", nvme_probe, nvme_remove);
    kstatus_t status = driver_register(&g_nvme_driver);
    atomic_store_explicit(&g_nvme_driver_state, 2U, memory_order_release);
    return status;
}

bool nvme_driver_self_test(void) {
    if (nvme_driver_register() != K_OK) return false;
    if (g_nvme_hardware_seen && nvme_active_controller() == 0) return false;
    for (uint32_t i = 0; i < NVME_MAX_CONTROLLERS; ++i) {
        nvme_controller_t *controller = g_nvme_controllers[i];
        if (controller != 0 && (!controller->started || !controller->identified)) return false;
    }
    return true;
}

const nvme_controller_t *nvme_active_controller(void) {
    for (uint32_t i = 0; i < NVME_MAX_CONTROLLERS; ++i) {
        if (g_nvme_controllers[i] != 0 && g_nvme_controllers[i]->started) {
            return g_nvme_controllers[i];
        }
    }
    return 0;
}

bool nvme_hardware_present(void) {
    return g_nvme_hardware_seen;
}

kstatus_t nvme_last_error(void) {
    return g_nvme_last_status;
}

uint32_t nvme_last_stage(void) {
    return g_nvme_last_stage;
}

uint16_t nvme_last_completion_status(void) {
    return g_nvme_last_completion;
}
