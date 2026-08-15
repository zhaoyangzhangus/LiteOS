#include <arch/x86_64/uaccess.h>
#include <kernel/futex.h>
#include <kernel/kmem.h>
#include <kernel/sched.h>
#include <kernel/wait.h>

#define FUTEX_BUCKET_COUNT 256U

typedef struct futex_entry {
    list_head_t node;
    process_t *process;
    vaddr_t address;
    uint32_t refs;
    wait_queue_t waitq;
} futex_entry_t;

typedef struct {
    spinlock_t lock;
    list_head_t entries;
} futex_bucket_t;

typedef struct {
    uint32_t __user *address;
    uint32_t expected;
    bool checked;
    bool mismatch;
    kstatus_t fault;
} futex_predicate_t;

static futex_bucket_t g_futex_buckets[FUTEX_BUCKET_COUNT];
static atomic_uint g_futex_init_state;

static void futex_lock(spinlock_t *lock) {
    while (atomic_exchange_explicit(&lock->state, 1U, memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
}

static void futex_unlock(spinlock_t *lock) {
    atomic_store_explicit(&lock->state, 0U, memory_order_release);
}

static void list_insert_tail(list_head_t *head, list_head_t *node) {
    node->next = head;
    node->prev = head->prev;
    head->prev->next = node;
    head->prev = node;
}

static void list_remove_node(list_head_t *node) {
    node->prev->next = node->next;
    node->next->prev = node->prev;
    list_init(node);
}

static futex_entry_t *entry_from_node(list_head_t *node) {
    return (futex_entry_t *)((uint8_t *)node -
                             __builtin_offsetof(futex_entry_t, node));
}

static void futex_init(void) {
    unsigned expected = 0;
    if (atomic_compare_exchange_strong_explicit(&g_futex_init_state, &expected, 1U,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        for (uint32_t i = 0; i < FUTEX_BUCKET_COUNT; ++i) {
            atomic_init(&g_futex_buckets[i].lock.state, 0U);
            list_init(&g_futex_buckets[i].entries);
        }
        atomic_store_explicit(&g_futex_init_state, 2U, memory_order_release);
        return;
    }
    while (atomic_load_explicit(&g_futex_init_state, memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
}

static uint32_t futex_hash(process_t *process, vaddr_t address) {
    uint64_t value = ((uint64_t)(uintptr_t)process >> 4) ^
                     ((uint64_t)address >> 2) ^ ((uint64_t)address >> 21);
    value ^= value >> 32;
    return (uint32_t)value & (FUTEX_BUCKET_COUNT - 1U);
}

static futex_entry_t *find_locked(futex_bucket_t *bucket, process_t *process,
                                  vaddr_t address) {
    for (list_head_t *node = bucket->entries.next; node != &bucket->entries;
         node = node->next) {
        futex_entry_t *entry = entry_from_node(node);
        if (entry->process == process && entry->address == address) return entry;
    }
    return 0;
}

static kstatus_t futex_get(process_t *process, vaddr_t address, bool create,
                           futex_entry_t **out) {
    uint32_t index = futex_hash(process, address);
    futex_bucket_t *bucket = &g_futex_buckets[index];
    futex_entry_t *candidate = 0;
    for (;;) {
        futex_lock(&bucket->lock);
        futex_entry_t *entry = find_locked(bucket, process, address);
        if (entry != 0) {
            ++entry->refs;
            futex_unlock(&bucket->lock);
            kfree(candidate);
            *out = entry;
            return K_OK;
        }
        if (!create) {
            futex_unlock(&bucket->lock);
            kfree(candidate);
            return K_ENOENT;
        }
        if (candidate != 0) {
            candidate->process = process;
            candidate->address = address;
            candidate->refs = 1U;
            object_get(process);
            wait_queue_init(&candidate->waitq);
            list_insert_tail(&bucket->entries, &candidate->node);
            futex_unlock(&bucket->lock);
            *out = candidate;
            return K_OK;
        }
        futex_unlock(&bucket->lock);
        candidate = (futex_entry_t *)kzalloc(sizeof(*candidate), 0);
        if (candidate == 0) return K_ENOMEM;
        list_init(&candidate->node);
    }
}

static void futex_put(futex_entry_t *entry) {
    uint32_t index = futex_hash(entry->process, entry->address);
    futex_bucket_t *bucket = &g_futex_buckets[index];
    bool destroy = false;
    futex_lock(&bucket->lock);
    if (entry->refs != 0 && --entry->refs == 0) {
        list_remove_node(&entry->node);
        destroy = true;
    }
    futex_unlock(&bucket->lock);
    if (destroy) {
        object_put(entry->process);
        kfree(entry);
    }
}

static bool futex_predicate(void *raw_context) {
    futex_predicate_t *context = (futex_predicate_t *)raw_context;
    if (context->checked) return true; /* 被显式唤醒后允许伪唤醒语义。 */
    context->checked = true;
    uint32_t value = 0;
    context->fault = get_user_u32(&value, context->address);
    context->mismatch = context->fault == K_OK && value != context->expected;
    return context->fault != K_OK || context->mismatch;
}

kstatus_t futex_wait(process_t *process, uint32_t __user *address,
                     uint32_t expected, uint64_t timeout_ns) {
    if (process == 0 || address == 0 || ((uintptr_t)address & 3U) != 0) return K_EINVAL;
    futex_init();
    futex_entry_t *entry = 0;
    kstatus_t status = futex_get(process, (vaddr_t)(uintptr_t)address, true, &entry);
    if (status != K_OK) return status;
    futex_predicate_t predicate = {
        .address = address,
        .expected = expected,
        .checked = false,
        .mismatch = false,
        .fault = K_OK,
    };
    status = wait_on_queue(&entry->waitq, futex_predicate, &predicate, timeout_ns);
    futex_put(entry);
    if (predicate.fault != K_OK) return predicate.fault;
    if (predicate.mismatch) return K_EAGAIN;
    return status;
}

kstatus_t futex_wake(process_t *process, uint32_t __user *address,
                     uint32_t maximum, uint32_t *woken) {
    if (process == 0 || address == 0 || woken == 0 ||
        ((uintptr_t)address & 3U) != 0) return K_EINVAL;
    futex_init();
    *woken = 0;
    if (maximum == 0) return K_OK;
    futex_entry_t *entry = 0;
    /* 唤醒方也必须创建桶项，避免等待方首次建项时产生 lost wakeup。 */
    kstatus_t status = futex_get(process, (vaddr_t)(uintptr_t)address, true, &entry);
    if (status != K_OK) return status;
    while (*woken < maximum && wake_one(&entry->waitq) != 0) ++*woken;
    futex_put(entry);
    return K_OK;
}
