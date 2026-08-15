#include <kernel/object.h>
#include <kernel/wait.h>

static wait_queue_t g_object_signal_queue;
static atomic_uint g_object_wait_init_state;

typedef struct {
    void *const *objects;
    size_t count;
    bool wait_all;
    uint32_t signaled_index;
} object_wait_context_t;

static void object_wait_initialize(void) {
    unsigned expected = 0U;
    if (atomic_compare_exchange_strong_explicit(&g_object_wait_init_state, &expected, 1U,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        wait_queue_init(&g_object_signal_queue);
        atomic_store_explicit(&g_object_wait_init_state, 2U, memory_order_release);
        return;
    }
    while (atomic_load_explicit(&g_object_wait_init_state, memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
}

void object_get(void *object) {
    if (object == 0) return;
    object_header_t *header = (object_header_t *)object;
    atomic_fetch_add_explicit(&header->refs.value, 1U, memory_order_relaxed);
}

bool object_try_get(void *object) {
    if (object == 0) return false;
    object_header_t *header = (object_header_t *)object;
    unsigned count = atomic_load_explicit(&header->refs.value, memory_order_acquire);
    while (count != 0U) {
        if (atomic_compare_exchange_weak_explicit(&header->refs.value, &count, count + 1U,
                                                   memory_order_acquire, memory_order_relaxed)) return true;
    }
    return false;
}

void object_put(void *object) {
    if (object == 0) return;
    object_header_t *header = (object_header_t *)object;
    if (atomic_fetch_sub_explicit(&header->refs.value, 1U, memory_order_acq_rel) == 1U &&
        header->ops != 0 && header->ops->destroy != 0) {
        header->ops->destroy(object);
    }
}

bool object_is_signaled(const void *object) {
    if (object == 0) return false;
    const object_header_t *header = (const object_header_t *)object;
    return header->ops != 0 && header->ops->is_signaled != 0 &&
           header->ops->is_signaled(object);
}

int64_t object_wait_value(const void *object) {
    if (object == 0) return K_EINVAL;
    const object_header_t *header = (const object_header_t *)object;
    return header->ops != 0 && header->ops->wait_value != 0 ?
           header->ops->wait_value(object) : 0;
}

void object_notify_signaled(void *object) {
    if (object == 0 || !object_is_signaled(object)) return;
    object_wait_initialize();
    (void)wake_all(&g_object_signal_queue);
}

static bool object_wait_predicate(void *opaque) {
    object_wait_context_t *context = (object_wait_context_t *)opaque;
    if (context->wait_all) {
        for (size_t i = 0; i < context->count; ++i) {
            if (!object_is_signaled(context->objects[i])) return false;
        }
        context->signaled_index = UINT32_MAX;
        return true;
    }
    for (size_t i = 0; i < context->count; ++i) {
        if (object_is_signaled(context->objects[i])) {
            context->signaled_index = (uint32_t)i;
            return true;
        }
    }
    return false;
}

kstatus_t object_wait_many(void *const *objects, size_t count, bool wait_all,
                           uint64_t timeout_ns, uint32_t *signaled_index,
                           int64_t *wait_value) {
    if (objects == 0 || count == 0 || count > UINT32_MAX || signaled_index == 0 ||
        wait_value == 0) return K_EINVAL;
    for (size_t i = 0; i < count; ++i) {
        const object_header_t *header = (const object_header_t *)objects[i];
        if (header == 0 || header->ops == 0 || header->ops->is_signaled == 0) {
            return K_EINVAL;
        }
    }
    object_wait_initialize();
    object_wait_context_t context = {
        .objects = objects,
        .count = count,
        .wait_all = wait_all,
        .signaled_index = UINT32_MAX,
    };
    kstatus_t status = wait_on_queue(&g_object_signal_queue, object_wait_predicate,
                                     &context, timeout_ns);
    if (status != K_OK) return status;
    *signaled_index = context.signaled_index;
    *wait_value = wait_all ? 0 : object_wait_value(objects[context.signaled_index]);
    return K_OK;
}
