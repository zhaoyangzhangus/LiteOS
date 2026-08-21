#include <stdint.h>

typedef __SIZE_TYPE__ size_t;

void *memcpy(void *destination, const void *source, size_t length) {
    uint8_t *out = (uint8_t *)destination;
    const uint8_t *in = (const uint8_t *)source;
    while (length-- != 0U) *out++ = *in++;
    return destination;
}

void *memmove(void *destination, const void *source, size_t length) {
    uint8_t *out = (uint8_t *)destination;
    const uint8_t *in = (const uint8_t *)source;
    if (out == in || length == 0U) return destination;
    if (out < in || out >= in + length) {
        while (length-- != 0U) *out++ = *in++;
    } else {
        out += length;
        in += length;
        while (length-- != 0U) *--out = *--in;
    }
    return destination;
}

void *memset(void *destination, int value, size_t length) {
    uint8_t *out = (uint8_t *)destination;
    while (length-- != 0U) *out++ = (uint8_t)value;
    return destination;
}

int memcmp(const void *left, const void *right, size_t length) {
    const uint8_t *a = (const uint8_t *)left;
    const uint8_t *b = (const uint8_t *)right;
    while (length-- != 0U) {
        if (*a != *b) return *a < *b ? -1 : 1;
        ++a;
        ++b;
    }
    return 0;
}

size_t strlen(const char *text) {
    size_t length = 0U;
    if (text == 0) return 0U;
    while (text[length] != '\0') ++length;
    return length;
}
