#include <arch/x86_64/paging.h>
#include <arch/x86_64/uaccess.h>
#include <kernel/futex.h>
#include <kernel/kmem.h>
#include <kernel/sched.h>
#include <kernel/wait.h>
#include <kernel/vm.h>

#define FUTEX_BUCKET_COUNT 256U
#define FUTEX_USER_BASE 0x0000000000010000ULL
#define FUTEX_USER_END  (X86_64_USER_TOP + 1ULL)

typedef struct futex_entry {
    list_head_t node;
    /* The physical word is the identity shared by process aliases. */
    uint64_t key;
    process_t *process;
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

/*
 * Futex bucket ownership protects the entry list.  The lock is also taken
 * from the wake path on a different CPU, so a holder must not be switched
 * away while another CPU is spinning for it.
 */
static void futex_bucket_lock(spinlock_t *lock) {
    sched_preempt_disable();
    spinlock_lock(lock);
}

static void futex_bucket_unlock(spinlock_t *lock) {
    spinlock_unlock(lock);
    sched_preempt_enable();
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

static uint32_t futex_hash(uint64_t key) {
    uint64_t value = key >> 2;
    value ^= key >> 21;
    value ^= value >> 32;
    return (uint32_t)value & (FUTEX_BUCKET_COUNT - 1U);
}

static futex_entry_t *find_locked(futex_bucket_t *bucket, uint64_t key) {
    for (list_head_t *node = bucket->entries.next; node != &bucket->entries;
         node = node->next) {
        futex_entry_t *entry = entry_from_node(node);
        if (entry->key == key) return entry;
    }
    return 0;
}

static kstatus_t futex_resolve_key(process_t *process, vaddr_t address,
                                   uint64_t *out_key) {
    if (process == 0 || process->vm == 0 || out_key == 0 ||
        address < FUTEX_USER_BASE || address >= FUTEX_USER_END) {
        return K_EACCES;
    }

    paddr_t physical;
    kstatus_t status = x86_translate_page(process->vm->root_table, address,
                                           &physical, 0);
    if (status != K_OK) {
        status = vm_handle_fault(process->vm, &(vm_fault_info_t){
            .address = address,
            .access = VM_PROT_READ,
            .cpu_error = 0,
        });
        if (status != K_OK) return status;
        status = x86_translate_page(process->vm->root_table, address,
                                    &physical, 0);
        if (status != K_OK) return status;
    }

    /* Include the byte offset so different words in one shared page do not
     * wake each other.  Callers already enforce 4-byte user alignment. */
    *out_key = physical.value;
    return K_OK;
}

static kstatus_t futex_get(process_t *process, vaddr_t address, bool create,
                           futex_entry_t **out) {
    uint64_t key;
    kstatus_t status = futex_resolve_key(process, address, &key);
    if (status != K_OK) return status;
    uint32_t index = futex_hash(key);
    futex_bucket_t *bucket = &g_futex_buckets[index];
    futex_entry_t *candidate = 0;
    for (;;) {
        futex_bucket_lock(&bucket->lock);
        futex_entry_t *entry = find_locked(bucket, key);
        if (entry != 0) {
            ++entry->refs;
            futex_bucket_unlock(&bucket->lock);
            kfree(candidate);
            *out = entry;
            return K_OK;
        }
        if (!create) {
            futex_bucket_unlock(&bucket->lock);
            kfree(candidate);
            return K_ENOENT;
        }
        if (candidate != 0) {
            candidate->key = key;
            candidate->process = process;
            candidate->refs = 1U;
            object_get(process);
            wait_queue_init(&candidate->waitq);
            list_add_tail(&bucket->entries, &candidate->node);
            futex_bucket_unlock(&bucket->lock);
            *out = candidate;
            return K_OK;
        }
        futex_bucket_unlock(&bucket->lock);
        candidate = (futex_entry_t *)kzalloc(sizeof(*candidate), 0);
        if (candidate == 0) return K_ENOMEM;
        list_init(&candidate->node);
    }
}

static void futex_put(futex_entry_t *entry) {
    uint32_t index = futex_hash(entry->key);
    futex_bucket_t *bucket = &g_futex_buckets[index];
    bool destroy = false;
    futex_bucket_lock(&bucket->lock);
    if (entry->refs != 0 && --entry->refs == 0) {
        list_del(&entry->node);
        destroy = true;
    }
    futex_bucket_unlock(&bucket->lock);
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
