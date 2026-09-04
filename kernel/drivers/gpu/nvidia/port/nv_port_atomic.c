#include "nv_port.h"

void nv_port_atomic32_init(nv_port_atomic32_t *value, uint32_t initial) {
    if (value != 0) atomic_init(&value->value, initial);
}

uint32_t nv_port_atomic32_load(const nv_port_atomic32_t *value) {
    return value == 0 ? 0U : atomic_load_explicit(&value->value,
                                                   memory_order_acquire);
}

void nv_port_atomic32_store(nv_port_atomic32_t *value, uint32_t next) {
    if (value != 0) atomic_store_explicit(&value->value, next,
                                          memory_order_release);
}

uint32_t nv_port_atomic32_fetch_add(nv_port_atomic32_t *value,
                                    uint32_t delta) {
    return value == 0 ? 0U : atomic_fetch_add_explicit(&value->value, delta,
                                                        memory_order_acq_rel);
}

uint32_t nv_port_atomic32_fetch_sub(nv_port_atomic32_t *value,
                                    uint32_t delta) {
    return value == 0 ? 0U : atomic_fetch_sub_explicit(&value->value, delta,
                                                        memory_order_acq_rel);
}

bool nv_port_atomic32_compare_exchange(nv_port_atomic32_t *value,
                                       uint32_t *expected, uint32_t desired) {
    if (value == 0 || expected == 0) return false;
    return atomic_compare_exchange_strong_explicit(
        &value->value, expected, desired, memory_order_acq_rel,
        memory_order_acquire);
}

void nv_port_atomic64_init(nv_port_atomic64_t *value, uint64_t initial) {
    if (value != 0) atomic_init(&value->value, initial);
}

uint64_t nv_port_atomic64_load(const nv_port_atomic64_t *value) {
    return value == 0 ? 0U : atomic_load_explicit(&value->value,
                                                   memory_order_acquire);
}

void nv_port_atomic64_store(nv_port_atomic64_t *value, uint64_t next) {
    if (value != 0) atomic_store_explicit(&value->value, next,
                                          memory_order_release);
}

uint64_t nv_port_atomic64_fetch_add(nv_port_atomic64_t *value,
                                    uint64_t delta) {
    return value == 0 ? 0U : atomic_fetch_add_explicit(&value->value, delta,
                                                        memory_order_acq_rel);
}

uint64_t nv_port_atomic64_fetch_sub(nv_port_atomic64_t *value,
                                    uint64_t delta) {
    return value == 0 ? 0U : atomic_fetch_sub_explicit(&value->value, delta,
                                                        memory_order_acq_rel);
}

bool nv_port_atomic64_compare_exchange(nv_port_atomic64_t *value,
                                       uint64_t *expected, uint64_t desired) {
    if (value == 0 || expected == 0) return false;
    return atomic_compare_exchange_strong_explicit(
        &value->value, expected, desired, memory_order_acq_rel,
        memory_order_acquire);
}

bool nv_port_atomic_self_test(void) {
    nv_port_atomic32_t value32;
    nv_port_atomic64_t value64;
    uint32_t expected32 = 12U;
    uint64_t expected64 = 20U;

    nv_port_atomic32_init(&value32, 7U);
    nv_port_atomic64_init(&value64, 11U);
    if (nv_port_atomic32_load(&value32) != 7U ||
        nv_port_atomic64_load(&value64) != 11U ||
        nv_port_atomic32_fetch_add(&value32, 5U) != 7U ||
        nv_port_atomic64_fetch_add(&value64, 9U) != 11U ||
        nv_port_atomic32_load(&value32) != 12U ||
        nv_port_atomic64_load(&value64) != 20U ||
        !nv_port_atomic32_compare_exchange(&value32, &expected32, 13U) ||
        !nv_port_atomic64_compare_exchange(&value64, &expected64, 21U) ||
        nv_port_atomic32_fetch_sub(&value32, 3U) != 13U ||
        nv_port_atomic64_fetch_sub(&value64, 4U) != 21U) {
        return false;
    }

    expected32 = 99U;
    expected64 = 99U;
    if (nv_port_atomic32_compare_exchange(&value32, &expected32, 0U) ||
        nv_port_atomic64_compare_exchange(&value64, &expected64, 0U) ||
        expected32 != 10U || expected64 != 17U) {
        return false;
    }
    return nv_port_atomic32_load(&value32) == 10U &&
           nv_port_atomic64_load(&value64) == 17U;
}
