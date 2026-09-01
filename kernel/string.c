#include <stddef.h>

/* Freestanding kernel replacements for compiler-emitted byte operations. */
void *memset(void *destination, int value, size_t length) {
    unsigned char *bytes = (unsigned char *)destination;
    unsigned char byte = (unsigned char)value;
    for (size_t index = 0U; index < length; ++index) bytes[index] = byte;
    return destination;
}

void *memcpy(void *destination, const void *source, size_t length) {
    unsigned char *out = (unsigned char *)destination;
    const unsigned char *in = (const unsigned char *)source;
    for (size_t index = 0U; index < length; ++index) out[index] = in[index];
    return destination;
}

void *memmove(void *destination, const void *source, size_t length) {
    unsigned char *out = (unsigned char *)destination;
    const unsigned char *in = (const unsigned char *)source;
    if (out == in || length == 0U) return destination;
    if (out < in || out >= in + length) {
        for (size_t index = 0U; index < length; ++index) {
            out[index] = in[index];
        }
    } else {
        for (size_t index = length; index != 0U; --index) {
            out[index - 1U] = in[index - 1U];
        }
    }
    return destination;
}

int memcmp(const void *left, const void *right, size_t length) {
    const unsigned char *a = (const unsigned char *)left;
    const unsigned char *b = (const unsigned char *)right;
    for (size_t index = 0U; index < length; ++index) {
        if (a[index] != b[index]) {
            return a[index] < b[index] ? -1 : 1;
        }
    }
    return 0;
}
