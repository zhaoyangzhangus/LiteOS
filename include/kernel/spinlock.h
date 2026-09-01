#pragma once
#pragma once
#include "base.h"

typedef struct spinlock {
    atomic_uint state;
} spinlock_t;

typedef struct rwlock {
    atomic_uint state;
} rwlock_t;

typedef uint64_t irq_flags_t;

static inline void spinlock_init(spinlock_t *lock) {
    atomic_init(&lock->state, 0U);
}

static inline void spinlock_lock(spinlock_t *lock) {
    while (atomic_exchange_explicit(&lock->state, 1U,
                                    memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
}

static inline bool spinlock_try_lock(spinlock_t *lock) {
    unsigned expected = 0U;
    return atomic_compare_exchange_strong_explicit(
        &lock->state, &expected, 1U, memory_order_acquire,
        memory_order_relaxed);
}

static inline void spinlock_unlock(spinlock_t *lock) {
    atomic_store_explicit(&lock->state, 0U, memory_order_release);
}
