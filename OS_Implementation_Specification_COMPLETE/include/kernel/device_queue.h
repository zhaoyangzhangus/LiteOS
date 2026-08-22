#pragma once

#include "base.h"

/*
 * LiteOS hardware queue primitive.
 *
 * This is intentionally only an SPSC byte-entry ring. The queue does not
 * prescribe a universal device message ABI: every driver chooses the smallest
 * command/completion/event structure that matches its hardware.
 *
 * Typical ownership:
 *
 *   command queue:     one software producer -> one device worker consumer
 *   completion queue:  one ISR producer      -> one device worker consumer
 *   event queue:       one ISR producer      -> one subsystem consumer
 *
 * If a device needs multiple MSI-X vectors, give each producer its own queue
 * instead of turning this primitive into a contended MPSC ring.
 */
typedef struct device_queue {
    uint8_t *entries;
    uint32_t entry_size;
    uint32_t capacity;
    uint32_t mask;
    uint32_t reserved;

    /* Producer/consumer counters are isolated onto separate cache lines. */
    atomic_uint producer __aligned(CACHELINE_SIZE);
    atomic_uint consumer __aligned(CACHELINE_SIZE);
} device_queue_t;

/* capacity must be a power of two. Storage is owned by the caller. */
kstatus_t device_queue_init(device_queue_t *queue, void *storage,
                            uint32_t capacity, uint32_t entry_size);

/* Reset is only legal when no producer/consumer is concurrently using it. */
void device_queue_reset(device_queue_t *queue);

uint32_t device_queue_count(const device_queue_t *queue);
bool device_queue_empty(const device_queue_t *queue);
bool device_queue_full(const device_queue_t *queue);

/*
 * Zero-copy hot path. Exactly one producer and one consumer may operate on a
 * queue. The producer writes the reserved slot completely before commit; the
 * consumer must finish reading the peeked slot before release.
 */
static inline void *device_queue_producer_reserve(device_queue_t *queue) {
    uint32_t producer;
    uint32_t consumer;

    if (queue == 0 || queue->entries == 0 || queue->capacity == 0U) return 0;

    producer = atomic_load_explicit(&queue->producer, memory_order_relaxed);
    consumer = atomic_load_explicit(&queue->consumer, memory_order_acquire);
    if ((uint32_t)(producer - consumer) >= queue->capacity) return 0;

    return queue->entries +
           (size_t)(producer & queue->mask) * (size_t)queue->entry_size;
}

static inline void device_queue_producer_commit(device_queue_t *queue) {
    uint32_t producer =
        atomic_load_explicit(&queue->producer, memory_order_relaxed);

    atomic_store_explicit(&queue->producer, producer + 1U,
                          memory_order_release);
}

static inline const void *device_queue_consumer_peek(
    const device_queue_t *queue) {
    uint32_t consumer;
    uint32_t producer;

    if (queue == 0 || queue->entries == 0 || queue->capacity == 0U) return 0;

    consumer = atomic_load_explicit(&queue->consumer, memory_order_relaxed);
    producer = atomic_load_explicit(&queue->producer, memory_order_acquire);
    if (consumer == producer) return 0;

    return queue->entries +
           (size_t)(consumer & queue->mask) * (size_t)queue->entry_size;
}

static inline void device_queue_consumer_release(device_queue_t *queue) {
    uint32_t consumer =
        atomic_load_explicit(&queue->consumer, memory_order_relaxed);

    atomic_store_explicit(&queue->consumer, consumer + 1U,
                          memory_order_release);
}

/* Convenience copies for cold/control paths. Hot ISRs should reserve/commit. */
bool device_queue_try_push(device_queue_t *queue, const void *entry);
bool device_queue_try_pop(device_queue_t *queue, void *entry);

bool device_queue_self_test(void);
