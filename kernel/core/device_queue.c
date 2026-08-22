#include <kernel/device_queue.h>

static bool device_queue_power_of_two(uint32_t value) {
    return value != 0U && (value & (value - 1U)) == 0U;
}

/* The kernel is freestanding and does not provide libc memcpy. */
static void device_queue_copy(void *destination, const void *source,
                              uint32_t size) {
    uint8_t *to = (uint8_t *)destination;
    const uint8_t *from = (const uint8_t *)source;
    for (uint32_t index = 0U; index < size; ++index) to[index] = from[index];
}

kstatus_t device_queue_init(device_queue_t *queue, void *storage,
                            uint32_t capacity, uint32_t entry_size) {
    if (queue == 0 || storage == 0 || entry_size == 0U ||
        !device_queue_power_of_two(capacity) || capacity > 0x80000000U) {
        return K_EINVAL;
    }

    queue->entries = (uint8_t *)storage;
    queue->entry_size = entry_size;
    queue->capacity = capacity;
    queue->mask = capacity - 1U;
    queue->reserved = 0U;
    atomic_init(&queue->producer, 0U);
    atomic_init(&queue->consumer, 0U);
    return K_OK;
}

void device_queue_reset(device_queue_t *queue) {
    if (queue == 0) return;
    atomic_store_explicit(&queue->producer, 0U, memory_order_relaxed);
    atomic_store_explicit(&queue->consumer, 0U, memory_order_relaxed);
}

uint32_t device_queue_count(const device_queue_t *queue) {
    uint32_t producer;
    uint32_t consumer;
    uint32_t count;

    if (queue == 0 || queue->capacity == 0U) return 0U;
    producer = atomic_load_explicit(&queue->producer, memory_order_acquire);
    consumer = atomic_load_explicit(&queue->consumer, memory_order_acquire);
    count = (uint32_t)(producer - consumer);
    return count > queue->capacity ? queue->capacity : count;
}

bool device_queue_empty(const device_queue_t *queue) {
    if (queue == 0) return true;
    return atomic_load_explicit(&queue->producer, memory_order_acquire) ==
           atomic_load_explicit(&queue->consumer, memory_order_acquire);
}

bool device_queue_full(const device_queue_t *queue) {
    return queue != 0 && queue->capacity != 0U &&
           device_queue_count(queue) == queue->capacity;
}

bool device_queue_try_push(device_queue_t *queue, const void *entry) {
    void *destination;
    if (entry == 0) return false;
    destination = device_queue_producer_reserve(queue);
    if (destination == 0) return false;
    device_queue_copy(destination, entry, queue->entry_size);
    device_queue_producer_commit(queue);
    return true;
}

bool device_queue_try_pop(device_queue_t *queue, void *entry) {
    const void *source;
    if (entry == 0) return false;
    source = device_queue_consumer_peek(queue);
    if (source == 0) return false;
    device_queue_copy(entry, source, queue->entry_size);
    device_queue_consumer_release(queue);
    return true;
}

bool device_queue_self_test(void) {
    typedef struct test_entry {
        uint64_t sequence;
        uint32_t status;
        uint32_t value;
    } test_entry_t;

    device_queue_t queue;
    test_entry_t storage[4];
    test_entry_t out;

    if (device_queue_init(&queue, storage, 4U, sizeof(storage[0])) != K_OK)
        return false;
    if (!device_queue_empty(&queue) || device_queue_count(&queue) != 0U)
        return false;

    for (uint32_t index = 0U; index < 4U; ++index) {
        test_entry_t *entry =
            (test_entry_t *)device_queue_producer_reserve(&queue);
        if (entry == 0) return false;
        entry->sequence = (uint64_t)index + 1U;
        entry->status = index;
        entry->value = 0xA5000000U | index;
        device_queue_producer_commit(&queue);
    }

    if (!device_queue_full(&queue) || device_queue_count(&queue) != 4U ||
        device_queue_producer_reserve(&queue) != 0) return false;

    for (uint32_t index = 0U; index < 4U; ++index) {
        const test_entry_t *entry =
            (const test_entry_t *)device_queue_consumer_peek(&queue);
        if (entry == 0 || entry->sequence != (uint64_t)index + 1U ||
            entry->status != index ||
            entry->value != (0xA5000000U | index)) return false;
        device_queue_consumer_release(&queue);
    }

    if (!device_queue_empty(&queue)) return false;

    for (uint32_t index = 0U; index < 12U; ++index) {
        test_entry_t in = {
            .sequence = 100U + index,
            .status = 7U,
            .value = index,
        };
        if (!device_queue_try_push(&queue, &in) ||
            !device_queue_try_pop(&queue, &out) ||
            out.sequence != in.sequence || out.status != in.status ||
            out.value != in.value) return false;
    }

    device_queue_reset(&queue);
    return device_queue_empty(&queue) && device_queue_count(&queue) == 0U;
}
