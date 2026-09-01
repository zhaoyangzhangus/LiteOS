#pragma once
#pragma once
#include "base.h"

typedef struct refcount {
    atomic_uint value;
} refcount_t;

static inline void refcount_init(refcount_t *r, unsigned v) {
    atomic_init(&r->value, v);
}
