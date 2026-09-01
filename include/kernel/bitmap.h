#pragma once
#include "bitops.h"

static inline size_t bitmap_word_count(size_t bit_count) {
    return bit_count > (size_t)-1 - 63U ? 0U :
           (bit_count + 63U) / 64U;
}

static inline bool bitmap_test_bit(const uint64_t *words, size_t bit) {
    return words != 0 &&
           (words[bit / 64U] & bit_mask_u64((unsigned)(bit % 64U))) != 0U;
}

static inline void bitmap_set_bit(uint64_t *words, size_t bit) {
    words[bit / 64U] |= bit_mask_u64((unsigned)(bit % 64U));
}

static inline void bitmap_clear_bit(uint64_t *words, size_t bit) {
    words[bit / 64U] &= ~bit_mask_u64((unsigned)(bit % 64U));
}

static inline void bitmap_set_range(uint64_t *words, size_t start,
                                    size_t count, bool value) {
    for (size_t index = 0U; index < count; ++index) {
        if (value) bitmap_set_bit(words, start + index);
        else bitmap_clear_bit(words, start + index);
    }
}

static inline void bitmap_zero(uint64_t *words, size_t bit_count) {
    size_t words_count = bitmap_word_count(bit_count);
    while (words_count-- != 0U) words[words_count] = 0U;
}

static inline size_t bitmap_find_first_set(const uint64_t *words,
                                           size_t bit_count) {
    size_t words_count = bitmap_word_count(bit_count);
    for (size_t index = 0U; index < words_count; ++index) {
        uint64_t value = words[index];
        if (index + 1U == words_count && (bit_count & 63U) != 0U) {
            value &= (1ULL << (bit_count & 63U)) - 1ULL;
        }
        if (value != 0U) return index * 64U + bit_scan_forward_u64(value);
    }
    return (size_t)-1;
}

static inline size_t bitmap_find_first_zero(const uint64_t *words,
                                            size_t bit_count) {
    size_t words_count = bitmap_word_count(bit_count);
    for (size_t index = 0U; index < words_count; ++index) {
        uint64_t value = ~words[index];
        if (index + 1U == words_count && (bit_count & 63U) != 0U) {
            value &= (1ULL << (bit_count & 63U)) - 1ULL;
        }
        if (value != 0U) return index * 64U + bit_scan_forward_u64(value);
    }
    return (size_t)-1;
}
