#include <kernel/message_port.h>
#include <kernel/mm.h>

static void message_port_lock(message_port_t *port) {
    while (atomic_exchange_explicit(&port->lock.state, 1U,
                                    memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
}

static void message_port_unlock(message_port_t *port) {
    atomic_store_explicit(&port->lock.state, 0U, memory_order_release);
}

static bool message_port_ready(void *context) {
    message_port_t *port = (message_port_t *)context;
    bool ready;
    message_port_lock(port);
    ready = port->count != 0U ||
            atomic_load_explicit(&port->closed, memory_order_acquire);
    message_port_unlock(port);
    return ready;
}

static bool message_port_is_signaled(const void *object) {
    const message_port_t *port = (const message_port_t *)object;
    message_port_t *mutable_port = (message_port_t *)(uintptr_t)port;
    bool signaled;
    message_port_lock(mutable_port);
    signaled = mutable_port->count != 0U ||
               atomic_load_explicit(&mutable_port->closed, memory_order_acquire);
    message_port_unlock(mutable_port);
    return signaled;
}

static int64_t message_port_wait_value(const void *object) {
    const message_port_t *port = (const message_port_t *)object;
    message_port_t *mutable_port = (message_port_t *)(uintptr_t)port;
    int64_t value;
    message_port_lock(mutable_port);
    value = mutable_port->count;
    if (value == 0 && atomic_load_explicit(&mutable_port->closed, memory_order_acquire)) {
        value = K_ECANCELED;
    }
    message_port_unlock(mutable_port);
    return value;
}

static void message_port_destroy(void *object) {
    message_port_t *port = (message_port_t *)object;
    (void)message_port_close(port);
    kfree(port);
}

static const object_ops_t g_message_port_ops = {
    .destroy = message_port_destroy,
    .type_name = "MessagePort",
    .is_signaled = message_port_is_signaled,
    .wait_value = message_port_wait_value,
};

kstatus_t message_port_create(uint32_t capacity, message_port_t **out) {
    if (out == 0) return K_EINVAL;
    if (capacity == 0U) capacity = OS_PORT_DEFAULT_CAPACITY;
    if (capacity > OS_PORT_MAX_CAPACITY) return K_EINVAL;
    message_port_t *port = (message_port_t *)kzalloc(sizeof(*port), 0);
    if (port == 0) return K_ENOMEM;
    refcount_init(&port->object.refs, 1U);
    port->object.type = KOBJECT_TYPE_MESSAGE_PORT;
    port->object.flags = 0;
    port->object.ops = &g_message_port_ops;
    port->object.security = 0;
    atomic_init(&port->lock.state, 0U);
    wait_queue_init(&port->receive_waitq);
    atomic_init(&port->closed, false);
    port->capacity = capacity;
    *out = port;
    return K_OK;
}

kstatus_t message_port_send(message_port_t *port, const void *data, size_t size) {
    if (port == 0 || data == 0 || size == 0 || size > OS_PORT_MAX_MESSAGE_SIZE) {
        return K_EINVAL;
    }
    message_port_lock(port);
    if (atomic_load_explicit(&port->closed, memory_order_acquire)) {
        message_port_unlock(port);
        return K_ECANCELED;
    }
    if (port->count >= port->capacity) {
        message_port_unlock(port);
        return K_EAGAIN;
    }
    message_port_message_t *message = &port->messages[port->tail];
    message->size = (uint32_t)size;
    for (size_t i = 0; i < size; ++i) message->data[i] = ((const uint8_t *)data)[i];
    port->tail = (port->tail + 1U) % port->capacity;
    ++port->count;
    message_port_unlock(port);
    (void)wake_one(&port->receive_waitq);
    object_notify_signaled(port);
    return K_OK;
}

kstatus_t message_port_receive(message_port_t *port, void *data, size_t capacity,
                               size_t *size, uint64_t timeout_ns) {
    if (port == 0 || data == 0 || size == 0 || capacity == 0) return K_EINVAL;
    for (;;) {
        kstatus_t status = wait_on_queue(&port->receive_waitq, message_port_ready,
                                         port, timeout_ns);
        if (status != K_OK) return status;
        message_port_lock(port);
        if (port->count != 0U) {
            message_port_message_t *message = &port->messages[port->head];
            if (capacity < message->size) {
                message_port_unlock(port);
                return K_EINVAL;
            }
            for (uint32_t i = 0; i < message->size; ++i) {
                ((uint8_t *)data)[i] = message->data[i];
            }
            *size = message->size;
            port->head = (port->head + 1U) % port->capacity;
            --port->count;
            message_port_unlock(port);
            return K_OK;
        }
        bool closed = atomic_load_explicit(&port->closed, memory_order_acquire);
        message_port_unlock(port);
        if (closed) return K_ECANCELED;
    }
}

kstatus_t message_port_close(message_port_t *port) {
    if (port == 0) return K_EINVAL;
    bool was_closed = atomic_exchange_explicit(&port->closed, true,
                                               memory_order_acq_rel);
    (void)wake_all(&port->receive_waitq);
    if (!was_closed) object_notify_signaled(port);
    return K_OK;
}

bool message_port_self_test(void) {
    message_port_t *port = 0;
    const uint8_t sent[] = {'p', 'o', 'r', 't'};
    uint8_t received[sizeof(sent)] = {0};
    size_t size = 0;
    if (message_port_create(1U, &port) != K_OK || port == 0) return false;
    bool success = message_port_send(port, sent, sizeof(sent)) == K_OK &&
                   message_port_send(port, sent, sizeof(sent)) == K_EAGAIN &&
                   object_is_signaled(port) &&
                   message_port_receive(port, received, sizeof(received), &size, 0U) == K_OK &&
                   size == sizeof(sent) && received[0] == sent[0] &&
                   message_port_receive(port, received, sizeof(received), &size, 0U) == K_ETIMEDOUT &&
                   message_port_close(port) == K_OK &&
                   message_port_receive(port, received, sizeof(received), &size, 0U) == K_ECANCELED;
    object_put(port);
    return success;
}
