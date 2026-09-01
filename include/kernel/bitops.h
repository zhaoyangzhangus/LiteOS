#pragma once
#include "base.h"

static inline bool bit_test_u64(uint64_t value, unsigned bit) {
    return bit < 64U && (value & (1ULL << bit)) != 0U;
}

static inline uint64_t bit_mask_u64(unsigned bit) {
    return bit < 64U ? 1ULL << bit : 0U;
}

static inline unsigned bit_scan_forward_u64(uint64_t value) {
    return (unsigned)__builtin_ctzll(value);
}

static inline unsigned bit_scan_reverse_u64(uint64_t value) {
    return 63U - (unsigned)__builtin_clzll(value);
}

static inline unsigned bit_count_u64(uint64_t value) {
    return (unsigned)__builtin_popcountll(value);
}
