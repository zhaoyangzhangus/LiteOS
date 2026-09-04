#include <nv.h>
#include <nv-priv.h>

#include <kernel/deferred.h>

#include "../../port/include/nv_port.h"

/*
 * The 595.84 Unix RM passes an opaque queue to os_queue_work_item().  The
 * Phase 4 bridge deliberately leaves nv_state_t::queue NULL, which selects
 * the vendor global queue.  Back that queue with LiteOS' existing deferred worker
 * rather than introducing an NVIDIA-private scheduler.
 *
 * The pool size matches the vendor OS_MAX_QUEUED_WORKITEMS contract.  Keeping
 * both the dispatch record and its exact NVIDIA stack in the pool also makes
 * the enqueue path usable from atomic/interrupt callers without allocating
 * from the kernel heap there.
 */
#define NV_LITEOS_WORK_CAPACITY 32U

typedef struct nv_liteos_work_entry {
    nv_port_work_t work;
    nvidia_stack_t stack;
    void *data;
    atomic_bool in_use;
    atomic_bool completed;
} nv_liteos_work_entry_t;

static nv_port_once_t g_nv_liteos_work_once = { ATOMIC_VAR_INIT(0U) };
static nv_port_spinlock_t g_nv_liteos_work_lock;
static atomic_bool g_nv_liteos_work_flushing = ATOMIC_VAR_INIT(false);
static atomic_uint g_nv_liteos_work_publishers = ATOMIC_VAR_INIT(0U);
static nv_liteos_work_entry_t g_nv_liteos_work_entries[NV_LITEOS_WORK_CAPACITY];

static void nv_liteos_work_init_once(void *argument)
{
    uint32_t index;

    (void)argument;
    nv_port_spin_init(&g_nv_liteos_work_lock);
    atomic_init(&g_nv_liteos_work_flushing, false);
    atomic_init(&g_nv_liteos_work_publishers, 0U);
    for (index = 0U; index < NV_LITEOS_WORK_CAPACITY; ++index) {
        atomic_init(&g_nv_liteos_work_entries[index].in_use, false);
        atomic_init(&g_nv_liteos_work_entries[index].completed, false);
    }
}

static NV_STATUS nv_liteos_work_ready(void)
{
    return nv_port_once_run(&g_nv_liteos_work_once,
                            nv_liteos_work_init_once, NULL) == K_OK ?
           NV_OK : NV_ERR_OPERATING_SYSTEM;
}

static void nv_liteos_work_reclaim_locked(nv_liteos_work_entry_t *entry)
{
    unsigned state;

    if (entry == NULL ||
        !atomic_load_explicit(&entry->in_use, memory_order_acquire) ||
        !atomic_load_explicit(&entry->completed, memory_order_acquire))
        return;

    state = atomic_load_explicit(&entry->work.state, memory_order_acquire);
    if (state != NV_PORT_WORK_IDLE) return;

    entry->data = NULL;
    atomic_store_explicit(&entry->completed, false, memory_order_release);
    atomic_store_explicit(&entry->in_use, false, memory_order_release);
}

static nv_liteos_work_entry_t *nv_liteos_work_alloc_locked(void)
{
    uint32_t index;

    for (index = 0U; index < NV_LITEOS_WORK_CAPACITY; ++index) {
        nv_liteos_work_entry_t *entry = &g_nv_liteos_work_entries[index];

        nv_liteos_work_reclaim_locked(entry);
        if (!atomic_load_explicit(&entry->in_use, memory_order_acquire)) {
            atomic_store_explicit(&entry->in_use, true, memory_order_release);
            return entry;
        }
    }
    return NULL;
}

static void nv_liteos_work_execute(void *argument)
{
    nv_liteos_work_entry_t *entry = (nv_liteos_work_entry_t *)argument;

    if (entry == NULL) return;

    /* rm_execute_work_item() is the exact vendor dispatcher.  It owns the
     * nv_work_item_t and its pData according to the 595.84 contract. */
    rm_execute_work_item(&entry->stack, entry->data);
    entry->data = NULL;
    atomic_store_explicit(&entry->completed, true, memory_order_release);
}

NV_STATUS NV_API_CALL os_queue_work_item(struct os_work_queue *queue,
                                          void *data)
{
    nv_liteos_work_entry_t *entry;
    kstatus_t status;

    /* No per-GPU queue is created until a later lifecycle phase. */
    if (queue != NULL || data == NULL) return NV_ERR_NOT_SUPPORTED;
    if (nv_liteos_work_ready() != NV_OK) return NV_ERR_OPERATING_SYSTEM;

    nv_port_spin_lock(&g_nv_liteos_work_lock);
    if (atomic_load_explicit(&g_nv_liteos_work_flushing,
                             memory_order_acquire)) {
        nv_port_spin_unlock(&g_nv_liteos_work_lock);
        return NV_ERR_NOT_READY;
    }

    entry = nv_liteos_work_alloc_locked();
    if (entry == NULL) {
        nv_port_spin_unlock(&g_nv_liteos_work_lock);
        return NV_ERR_INSUFFICIENT_RESOURCES;
    }

    entry->data = data;
    entry->stack.size = (NvU32)sizeof(entry->stack.stack);
    entry->stack.top = entry->stack.stack + entry->stack.size;
    atomic_store_explicit(&entry->completed, false, memory_order_release);
    nv_port_work_init(&entry->work, nv_liteos_work_execute, entry);

    /* Publishers are tracked separately so the real enqueue can happen
     * without holding the pool lock.  A work callback is allowed to queue a
     * follow-up item; holding this lock across the enqueue would deadlock in
     * that valid vendor path. */
    atomic_fetch_add_explicit(&g_nv_liteos_work_publishers, 1U,
                              memory_order_acq_rel);
    nv_port_spin_unlock(&g_nv_liteos_work_lock);

    status = nv_port_work_queue(&entry->work);

    nv_port_spin_lock(&g_nv_liteos_work_lock);
    if (status != K_OK) {
        entry->data = NULL;
        atomic_store_explicit(&entry->in_use, false, memory_order_release);
    }
    atomic_fetch_sub_explicit(&g_nv_liteos_work_publishers, 1U,
                              memory_order_acq_rel);
    nv_port_spin_unlock(&g_nv_liteos_work_lock);
    return status == K_OK ? NV_OK :
           (status == K_ENOSPC ? NV_ERR_INSUFFICIENT_RESOURCES :
                                 NV_ERR_OPERATING_SYSTEM);
}

NV_STATUS NV_API_CALL os_flush_work_queue(struct os_work_queue *queue,
                                           NvBool is_unload)
{
    nv_liteos_work_entry_t *pending[NV_LITEOS_WORK_CAPACITY];
    uint32_t pending_count = 0U;
    uint32_t index;
    NV_STATUS result = NV_OK;

    (void)is_unload;
    if (queue != NULL) return NV_ERR_NOT_SUPPORTED;
    if (nv_liteos_work_ready() != NV_OK) return NV_ERR_OPERATING_SYSTEM;

    nv_port_spin_lock(&g_nv_liteos_work_lock);
    atomic_store_explicit(&g_nv_liteos_work_flushing, true,
                          memory_order_release);
    nv_port_spin_unlock(&g_nv_liteos_work_lock);

    /* A producer may have passed the flushing check just before it acquired
     * the pool lock.  Wait until that producer has either queued or released
     * its slot before taking the flush snapshot. */
    while (atomic_load_explicit(&g_nv_liteos_work_publishers,
                                memory_order_acquire) != 0U)
        __asm__ volatile ("pause");

    nv_port_spin_lock(&g_nv_liteos_work_lock);
    for (index = 0U; index < NV_LITEOS_WORK_CAPACITY; ++index) {
        nv_liteos_work_entry_t *entry = &g_nv_liteos_work_entries[index];

        if (atomic_load_explicit(&entry->in_use, memory_order_acquire))
            pending[pending_count++] = entry;
    }
    nv_port_spin_unlock(&g_nv_liteos_work_lock);

    for (index = 0U; index < pending_count; ++index) {
        if (nv_port_work_drain(&pending[index]->work, UINT64_MAX) != K_OK) {
            result = NV_ERR_OPERATING_SYSTEM;
            break;
        }
    }

    nv_port_spin_lock(&g_nv_liteos_work_lock);
    for (index = 0U; index < pending_count; ++index)
        nv_liteos_work_reclaim_locked(pending[index]);
    atomic_store_explicit(&g_nv_liteos_work_flushing, false,
                          memory_order_release);
    nv_port_spin_unlock(&g_nv_liteos_work_lock);
    return result;
}

NvBool NV_API_CALL os_is_queue_flush_ongoing(struct os_work_queue *queue)
{
    if (queue != NULL) return NV_FALSE;
    if (nv_liteos_work_ready() != NV_OK) return NV_TRUE;
    return atomic_load_explicit(&g_nv_liteos_work_flushing,
                                memory_order_acquire) ? NV_TRUE : NV_FALSE;
}
