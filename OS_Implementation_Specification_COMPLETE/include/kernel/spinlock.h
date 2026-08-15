#pragma once
#include "base.h"

typedef struct spinlock {
    atomic_uint state;
} spinlock_t;

typedef struct rwlock {
    atomic_uint state;
} rwlock_t;

typedef uint64_t irq_flags_t;
