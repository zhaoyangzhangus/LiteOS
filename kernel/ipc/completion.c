#include <kernel/completion_port.h>
#include <kernel/mm.h>

static void completion_port_lock(completion_port_t *port) {
    spinlock_lock(&port->lock);
}

static void completion_port_unlock(completion_port_t *port) {
    spinlock_unlock(&port->lock);
}

static bool completion_port_ready(void *context) {
    completion_port_t *port = (completion_port_t *)context;
    bool ready;
    completion_port_lock(port);
    ready = port->count != 0U ||
            atomic_load_explicit(&port->closed, memory_order_acquire);
    completion_port_unlock(port);
    return ready;
}

static bool completion_port_is_signaled(const void *object) {
    const completion_port_t *port = (const completion_port_t *)object;
    bool signaled;
    completion_port_t *mutable_port = (completion_port_t *)(uintptr_t)port;
    completion_port_lock(mutable_port);
    signaled = mutable_port->count != 0U ||
               atomic_load_explicit(&mutable_port->closed, memory_order_acquire);
    completion_port_unlock(mutable_port);
    return signaled;
}

static int64_t completion_port_wait_value(const void *object) {
    const completion_port_t *port = (const completion_port_t *)object;
    int64_t value;
    completion_port_t *mutable_port = (completion_port_t *)(uintptr_t)port;
    completion_port_lock(mutable_port);
    value = mutable_port->count;
    if (value == 0 && atomic_load_explicit(&mutable_port->closed, memory_order_acquire)) {
        value = K_ECANCELED;
    }
    completion_port_unlock(mutable_port);
    return value;
}

static void completion_port_destroy(void *object) {
    completion_port_t *port = (completion_port_t *)object;
    (void)completion_port_close(port);
    kfree(port);
}

static void completion_port_handle_close(void *object) {
    (void)completion_port_close((completion_port_t *)object);
}

static const object_ops_t g_completion_port_ops = {
    .destroy = completion_port_destroy,
    .handle_close = completion_port_handle_close,
    .type_name = "CompletionPort",
    .is_signaled = completion_port_is_signaled,
    .wait_value = completion_port_wait_value,
};

kstatus_t completion_port_create(uint32_t capacity, completion_port_t **out) {
    if (out == 0) return K_EINVAL;
    if (capacity == 0U) capacity = COMPLETION_PORT_DEFAULT_CAPACITY;
    if (capacity > COMPLETION_PORT_MAX_CAPACITY) return K_EINVAL;

    /* Capacity is bounded above before sizing. Every readable slot is fully
     * written by post before count publishes it under the port lock. */
    completion_port_t *port = (completion_port_t *)kmalloc(
        sizeof(*port) + (size_t)capacity * sizeof(port->entries[0]), 0);
    if (port == 0) return K_ENOMEM;
    object_header_init(&port->object, KOBJECT_TYPE_COMPLETION_PORT,
                       &g_completion_port_ops);
    spinlock_init(&port->lock);
    wait_queue_init(&port->waitq);
    atomic_init(&port->closed, false);
    port->capacity = capacity;
    port->head = 0U;
    port->tail = 0U;
    port->count = 0U;
    *out = port;
    return K_OK;
}

kstatus_t completion_port_post(completion_port_t *port,
                               const os_completion_entry_t *entry) {
    if (port == 0 || entry == 0) return K_EINVAL;
    completion_port_lock(port);
    if (atomic_load_explicit(&port->closed, memory_order_acquire)) {
        completion_port_unlock(port);
        return K_ECANCELED;
    }
    if (port->count >= port->capacity) {
        completion_port_unlock(port);
        return K_EAGAIN;
    }
    port->entries[port->tail] = *entry;
    if (++port->tail == port->capacity) port->tail = 0U;
    ++port->count;
    completion_port_unlock(port);
    (void)wake_one(&port->waitq);
    object_notify_signaled(port);
    return K_OK;
}

kstatus_t completion_port_wait(completion_port_t *port, uint64_t timeout_ns,
                               os_completion_entry_t *entry) {
    if (port == 0 || entry == 0) return K_EINVAL;
    for (;;) {
        /* Ready entries do not need a wait subscription/predicate lock.
         * The wait path rechecks readiness before it can sleep. */
        completion_port_lock(port);
        if (port->count != 0U) {
            *entry = port->entries[port->head];
            if (++port->head == port->capacity) port->head = 0U;
            --port->count;
            completion_port_unlock(port);
            return K_OK;
        }
        bool closed = atomic_load_explicit(&port->closed, memory_order_acquire);
        completion_port_unlock(port);
        if (closed) return K_ECANCELED;
        if (timeout_ns == 0U) return K_ETIMEDOUT;
        kstatus_t status = wait_on_queue(&port->waitq, completion_port_ready,
                                         port, timeout_ns);
        if (status != K_OK) return status;
    }
}

kstatus_t completion_port_close(completion_port_t *port) {
    if (port == 0) return K_EINVAL;
    bool was_closed = atomic_exchange_explicit(&port->closed, true,
                                               memory_order_acq_rel);
    /* Closed readiness is permanent. A later waiter rechecks the predicate
     * while subscribing; repeated closes need not rescan either wait queue. */
    if (!was_closed) {
        (void)wake_all(&port->waitq);
        object_notify_signaled(port);
    }
    return K_OK;
}

bool completion_port_self_test(void) {
    completion_port_t *port = 0;
    os_completion_entry_t first = {
        .user_key = 0x1111ULL,
        .status = K_OK,
        .bytes_done = 64U,
        .request_id = 1U,
    };
    os_completion_entry_t second = {
        .user_key = 0x2222ULL,
        .status = K_EIO,
        .bytes_done = 0U,
        .request_id = 2U,
    };
    os_completion_entry_t received = {0};
    if (completion_port_create(2U, &port) != K_OK || port == 0) return false;
    bool success = completion_port_post(port, &first) == K_OK &&
                   completion_port_post(port, &second) == K_OK &&
                   completion_port_post(port, &first) == K_EAGAIN &&
                   object_is_signaled(port) &&
                   completion_port_wait(port, 0U, &received) == K_OK &&
                   received.user_key == first.user_key &&
                   completion_port_wait(port, 0U, &received) == K_OK &&
                   received.user_key == second.user_key &&
                   completion_port_wait(port, 0U, &received) == K_ETIMEDOUT &&
                   completion_port_close(port) == K_OK &&
                   completion_port_wait(port, 0U, &received) == K_ECANCELED;
    uint32_t sequence = atomic_load_explicit(&port->waitq.sequence,
                                              memory_order_acquire);
    for (uint32_t i = 0U; success && i < 64U; ++i) {
        success = completion_port_close(port) == K_OK &&
                  atomic_load_explicit(&port->waitq.sequence,
                                       memory_order_acquire) == sequence;
    }
    object_put(port);
    return success;
}
