#include <kernel/string.h>

_Static_assert(sizeof(size_t) == sizeof(void *), "kernel string length ABI");

/* Freestanding kernel replacements for compiler-emitted byte operations. */
void *memset(void *destination, int value, size_t length) {
    unsigned char *bytes = (unsigned char *)destination;
    unsigned char byte = (unsigned char)value;
#if defined(__x86_64__)
    if (length >= 64U) {
        unsigned long long words = length / 8U;
        const unsigned long long pattern =
            (unsigned long long)byte * 0x0101010101010101ULL;
        /* Integer string instructions do not touch the kernel's FPU state.
         * Only complete words are accessed; the tail stays inside length. */
        __asm__ volatile("cld; rep stosq"
                         : "+D"(bytes), "+c"(words)
                         : "a"(pattern)
                         : "memory", "cc");
        length %= 8U;
    }
#endif
    for (size_t index = 0U; index < length; ++index) bytes[index] = byte;
    return destination;
}

/* Forward overlap is safe: each complete word is loaded before it is
 * stored, and a lower destination cannot overwrite a future source word.
 * This helper carries no memcpy non-overlap contract. */
static void copy_forward(void *destination, const void *source, size_t length) {
    unsigned char *out = (unsigned char *)destination;
    const unsigned char *in = (const unsigned char *)source;
#if defined(__x86_64__)
    if (length >= 64U) {
        unsigned long long words = length / 8U;
        __asm__ volatile("cld; rep movsq"
                         : "+D"(out), "+S"(in), "+c"(words)
                         :
                         : "memory", "cc");
        length %= 8U;
    }
#endif
    for (size_t index = 0U; index < length; ++index) out[index] = in[index];
}

void *memcpy(void *destination, const void *source, size_t length) {
    copy_forward(destination, source, length);
    return destination;
}

void *memmove(void *destination, const void *source, size_t length) {
    unsigned char *out = (unsigned char *)destination;
    const unsigned char *in = (const unsigned char *)source;
    if (out == in || length == 0U) return destination;
    uintptr_t to = (uintptr_t)out, from = (uintptr_t)in;
    if ((to < from && from - to >= length) ||
        (to >= from && to - from >= length)) {
        return memcpy(destination, source, length);
    }
    if (to < from) {
        copy_forward(destination, source, length);
    } else {
        /* Load the whole trailing word before storing it: even a one-byte
         * overlap cannot destroy source bytes needed by the next word.
         * Fixed-size copies tolerate alignment/aliasing without setting DF
         * or touching SIMD state; the remaining prefix is byte-sized. */
        if (length >= 64U) {
            while (length >= sizeof(uint64_t)) {
                uint64_t word;
                length -= sizeof(word);
                __builtin_memcpy(&word, in + length, sizeof(word));
                __builtin_memcpy(out + length, &word, sizeof(word));
            }
        }
        for (size_t index = length; index != 0U; --index) {
            out[index - 1U] = in[index - 1U];
        }
    }
    return destination;
}

int memcmp(const void *left, const void *right, size_t length) {
    const unsigned char *a = (const unsigned char *)left;
    const unsigned char *b = (const unsigned char *)right;
    /* Skip equal complete words without reading beyond the requested range.
     * Copies permit unaligned/aliased buffers; mismatches still use byte order
     * rather than the host integer ordering. */
    while (length >= sizeof(uint64_t)) {
        uint64_t word_a, word_b;
        __builtin_memcpy(&word_a, a, sizeof(word_a));
        __builtin_memcpy(&word_b, b, sizeof(word_b));
        if (word_a != word_b) break;
        a += sizeof(uint64_t);
        b += sizeof(uint64_t);
        length -= sizeof(uint64_t);
    }
    for (size_t index = 0U; index < length; ++index) {
        if (a[index] != b[index]) {
            return a[index] < b[index] ? -1 : 1;
        }
    }
    return 0;
}
