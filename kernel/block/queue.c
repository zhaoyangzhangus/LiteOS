#include <kernel/block.h>
#include <kernel/device.h>
#include <kernel/telemetry.h>
#include <arch/x86_64/acpi.h>
#include <kernel/wait.h>

#define BLOCK_DEVICE_QUEUE_LIMIT 64U
#define BLOCK_QUEUE_MAX_COUNT     8U

typedef struct block_queue_state {
    struct device *device;
    atomic_uint next_queue;
    uint16_t queue_count;
    bool used;
} block_queue_state_t;

static block_queue_state_t g_block_queues[BLOCK_DEVICE_QUEUE_LIMIT];
static spinlock_t g_block_lock;
static atomic_uint g_block_init_state;
static wait_queue_t g_block_completion_queue;

static bool block_request_completed(void *context) {
    io_request_t *request = (io_request_t *)context;
    if (request == 0) return true;
    /* 回调发生在 COMPLETING 阶段，必须等 finalize 写入终态后再返回。 */
    return io_request_is_terminal(request);
}

static void block_request_notify(void) {
    (void)wake_all(&g_block_completion_queue);
}

static void block_initialize(void) {
    unsigned expected = 0U;
    if (atomic_compare_exchange_strong_explicit(&g_block_init_state, &expected, 1U,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        atomic_init(&g_block_lock.state, 0U);
        wait_queue_init(&g_block_completion_queue);
        for (uint32_t i = 0; i < BLOCK_DEVICE_QUEUE_LIMIT; ++i) {
            g_block_queues[i].device = 0;
            g_block_queues[i].queue_count = 0;
            g_block_queues[i].used = false;
            atomic_init(&g_block_queues[i].next_queue, 0U);
        }
        atomic_store_explicit(&g_block_init_state, 2U, memory_order_release);
        return;
    }
    while (atomic_load_explicit(&g_block_init_state, memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
}

static block_queue_state_t *block_queue_for(struct device *device) {
    if (device == 0) return 0;
    block_initialize();
    while (atomic_exchange_explicit(&g_block_lock.state, 1U,
                                    memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
    block_queue_state_t *free_slot = 0;
    for (uint32_t i = 0; i < BLOCK_DEVICE_QUEUE_LIMIT; ++i) {
        if (g_block_queues[i].used && g_block_queues[i].device == device) {
            atomic_store_explicit(&g_block_lock.state, 0U, memory_order_release);
            return &g_block_queues[i];
        }
        if (!g_block_queues[i].used && free_slot == 0) free_slot = &g_block_queues[i];
    }
    if (free_slot != 0) {
        const x86_acpi_platform_t *platform = x86_acpi_platform();
        uint32_t count = platform != 0 ? platform->cpu_count : 1U;
        if (count == 0) count = 1U;
        if (count > BLOCK_QUEUE_MAX_COUNT) count = BLOCK_QUEUE_MAX_COUNT;
        free_slot->device = device;
        free_slot->queue_count = (uint16_t)count;
        free_slot->used = true;
        atomic_store_explicit(&free_slot->next_queue, 0U, memory_order_release);
    }
    atomic_store_explicit(&g_block_lock.state, 0U, memory_order_release);
    return free_slot;
}

static bool bio_page_vectors_valid(const bio_t *bio) {
    if (bio->vec_count != 0 && bio->vecs == 0) return false;
    uint64_t total = 0;
    for (uint16_t i = 0; i < bio->vec_count; ++i) {
        const bio_vec_t *vec = &bio->vecs[i];
        if (vec->page == 0 || (vec->page->flags & PAGE_FREE) != 0 ||
            (vec->page->flags & PAGE_COMPOUND_TAIL) != 0 ||
            vec->offset >= PAGE_SIZE || vec->length == 0 ||
            vec->length > PAGE_SIZE - vec->offset ||
            page_to_phys(vec->page).value == UINT64_MAX) return false;
        if (total > UINT64_MAX - vec->length) return false;
        total += vec->length;
    }
    return total != 0 || bio->op == BIO_OP_FLUSH;
}

static bool bio_request_ready(const bio_t *bio) {
    if (bio == 0 || bio->io == 0 || bio->io->device == 0 ||
        atomic_load_explicit(&bio->io->state, memory_order_acquire) != IOREQ_NEW ||
        bio->io->completion_target != 0) return false;
    return true;
}

kstatus_t block_submit_bio(bio_t *bio) {
    if (!bio_request_ready(bio) || !bio_page_vectors_valid(bio) ||
        (bio->op != BIO_OP_READ && bio->op != BIO_OP_WRITE &&
         bio->op != BIO_OP_FLUSH)) return K_EINVAL;
    if ((bio->op == BIO_OP_READ && bio->io->opcode != IO_READ) ||
        (bio->op == BIO_OP_WRITE && bio->io->opcode != IO_WRITE) ||
        (bio->op == BIO_OP_FLUSH && bio->io->opcode != IO_FLUSH)) return K_EINVAL;
    if (bio->op != BIO_OP_FLUSH && bio->io->vec_count == 0) return K_EINVAL;
    block_queue_state_t *queue = block_queue_for(bio->io->device);
    if (queue == 0 || queue->queue_count == 0) return K_ENOMEM;
    uint32_t selected = atomic_fetch_add_explicit(&queue->next_queue, 1U,
                                                  memory_order_relaxed) %
                        queue->queue_count;
    bio->io->flags = (bio->io->flags &
                      ~(IOREQ_BLOCK_QUEUE_VALID |
                        (IOREQ_BLOCK_QUEUE_MASK << IOREQ_BLOCK_QUEUE_SHIFT))) |
                     IOREQ_BLOCK_QUEUE_VALID |
                     ((selected & IOREQ_BLOCK_QUEUE_MASK) << IOREQ_BLOCK_QUEUE_SHIFT);
    /* 设备数据面通过此字段取得经过校验的 BIO，驱动完成前该对象必须保持有效。 */
    if (bio->io->completion_target != 0) return K_EBUSY;
    bio->io->completion_target = bio;
    bio->io->notify = block_request_notify;
    kstatus_t status = io_submit(bio->io);
    unsigned state = atomic_load_explicit(&bio->io->state, memory_order_acquire);
    if (!io_request_is_terminal(bio->io) && state != IOREQ_NEW) {
        status = wait_on_queue(&g_block_completion_queue, block_request_completed,
                               bio->io, UINT64_MAX);
        if (status == K_OK) status = bio->io->status;
    } else if (status == K_OK && io_request_is_terminal(bio->io)) {
        status = bio->io->status;
    }
    if (status != K_OK &&
        atomic_load_explicit(&bio->io->state, memory_order_acquire) == IOREQ_NEW &&
        bio->io->completion_target == bio) {
        bio->io->completion_target = 0;
    }
    return status;
}

kstatus_t block_submit_bio_batch(bio_t *bios, uint32_t count,
                                 uint32_t *submitted) {
    uint32_t completed = 0U;
    if (submitted == 0 || count == 0U || count > BLOCK_BIO_BATCH_MAX || bios == 0) {
        return K_EINVAL;
    }
    *submitted = 0U;
    /* 先完整校验，避免批量调用在第二个元素处才发现明显参数错误。 */
    for (uint32_t i = 0; i < count; ++i) {
        bio_t *bio = &bios[i];
        if (!bio_request_ready(bio) || !bio_page_vectors_valid(bio) ||
            (bio->op != BIO_OP_READ && bio->op != BIO_OP_WRITE &&
             bio->op != BIO_OP_FLUSH) ||
            (bio->op == BIO_OP_READ && bio->io->opcode != IO_READ) ||
            (bio->op == BIO_OP_WRITE && bio->io->opcode != IO_WRITE) ||
            (bio->op == BIO_OP_FLUSH && bio->io->opcode != IO_FLUSH)) {
            return K_EINVAL;
        }
    }
    for (; completed < count; ++completed) {
        kstatus_t status = block_submit_bio(&bios[completed]);
        if (status != K_OK) {
            *submitted = completed;
            (void)telemetry_record(TELEMETRY_CATEGORY_STORAGE_BATCH,
                                   bios[0].io->device->device_id, count, completed);
            return status;
        }
    }
    *submitted = completed;
    (void)telemetry_record(TELEMETRY_CATEGORY_STORAGE_BATCH,
                           bios[0].io->device->device_id, count, completed);
    return K_OK;
}

bool block_multiqueue_self_test(void) {
    static device_t first;
    static device_t second;
    block_queue_state_t *a = block_queue_for(&first);
    block_queue_state_t *b = block_queue_for(&second);
    if (a == 0 || b == 0 || a == b || a->queue_count == 0 || b->queue_count == 0) {
        return false;
    }
    uint32_t first_choice = atomic_fetch_add_explicit(&a->next_queue, 1U,
                                                       memory_order_relaxed) %
                            a->queue_count;
    uint32_t second_choice = atomic_fetch_add_explicit(&a->next_queue, 1U,
                                                        memory_order_relaxed) %
                             a->queue_count;
    return a->queue_count == 1U || first_choice != second_choice;
}
