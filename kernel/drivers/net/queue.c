#include "internal.h"

static void e1000_queue_lock(e1000_software_queue_t *queue) {
    while (atomic_exchange_explicit(&queue->lock.state, 1U,
                                    memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
}

static void e1000_queue_unlock(e1000_software_queue_t *queue) {
    atomic_store_explicit(&queue->lock.state, 0U, memory_order_release);
}

void e1000_queue_init(e1000_software_queue_t *queue) {
    if (queue == 0) return;
    atomic_init(&queue->lock.state, 0U);
    queue->head = 0U;
    queue->tail = 0U;
    queue->count = 0U;
    queue->dropped = 0U;
}

bool e1000_queue_push(e1000_software_queue_t *queue,
                      const e1000_rx_packet_t *packet) {
    if (queue == 0 || packet == 0 || packet->payload_length > SOCKET_MAX_PAYLOAD) {
        return false;
    }
    e1000_queue_lock(queue);
    if (queue->count >= E1000_SOFTWARE_QUEUE_DEPTH) {
        ++queue->dropped;
        e1000_queue_unlock(queue);
        return false;
    }
    __builtin_memcpy(&queue->packets[queue->tail], packet, sizeof(*packet));
    queue->tail = (queue->tail + 1U) % E1000_SOFTWARE_QUEUE_DEPTH;
    ++queue->count;
    e1000_queue_unlock(queue);
    return true;
}

bool e1000_queue_pop(e1000_software_queue_t *queue,
                     e1000_rx_packet_t *packet) {
    if (queue == 0 || packet == 0) return false;
    e1000_queue_lock(queue);
    if (queue->count == 0U) {
        e1000_queue_unlock(queue);
        return false;
    }
    __builtin_memcpy(packet, &queue->packets[queue->head], sizeof(*packet));
    queue->head = (queue->head + 1U) % E1000_SOFTWARE_QUEUE_DEPTH;
    --queue->count;
    e1000_queue_unlock(queue);
    return true;
}

bool e1000_packet_queue_self_test(void) {
    e1000_software_queue_t queue;
    e1000_rx_packet_t expected;
    e1000_rx_packet_t actual;

    e1000_queue_init(&queue);
    expected.sequence = 0U;
    expected.payload_length = 0U;
    for (uint32_t index = 0U; index < E1000_SOFTWARE_QUEUE_DEPTH; ++index) {
        expected.sequence = index + 1U;
        expected.payload_length = (uint16_t)(index + 1U);
        if (!e1000_queue_push(&queue, &expected)) return false;
    }
    if (e1000_queue_push(&queue, &expected) || queue.dropped != 1U) {
        return false;
    }
    for (uint32_t index = 0U; index < E1000_SOFTWARE_QUEUE_DEPTH; ++index) {
        if (!e1000_queue_pop(&queue, &actual) ||
            actual.sequence != index + 1U ||
            actual.payload_length != index + 1U) {
            return false;
        }
    }
    return !e1000_queue_pop(&queue, &actual) && queue.count == 0U;
}
