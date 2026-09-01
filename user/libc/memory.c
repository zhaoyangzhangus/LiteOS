#include "liteos/libc.h"

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

void *memccpy(void *destination, const void *source, int value, size_t length) {
    uint8_t *out = (uint8_t *)destination;
    const uint8_t *in = (const uint8_t *)source;
    uint8_t target = (uint8_t)value;
    while (length-- != 0U) {
        *out = *in++;
        if (*out++ == target) return out;
    }
    return 0;
}

void *memchr(const void *buffer, int value, size_t length) {
    const uint8_t *cursor = (const uint8_t *)buffer;
    uint8_t target = (uint8_t)value;
    if (cursor == 0) return 0;
    while (length-- != 0U) {
        if (*cursor == target) return (void *)cursor;
        ++cursor;
    }
    return 0;
}

void *memrchr(const void *buffer, int value, size_t length) {
    const uint8_t *cursor = (const uint8_t *)buffer;
    uint8_t target = (uint8_t)value;
    if (cursor == 0) return 0;
    while (length != 0U) {
        --length;
        if (cursor[length] == target) return (void *)(cursor + length);
    }
    return 0;
}

size_t strlen(const char *text) {
    size_t length = 0U;
    if (text == 0) return 0U;
    while (text[length] != '\0') ++length;
    return length;
}

size_t strnlen(const char *text, size_t limit) {
    size_t length = 0U;
    if (text == 0) return 0U;
    while (length < limit && text[length] != '\0') ++length;
    return length;
}

int strcmp(const char *left, const char *right) {
    if (left == 0 || right == 0) return left == right ? 0 : (left == 0 ? -1 : 1);
    while (*left != '\0' && *left == *right) {
        ++left;
        ++right;
    }
    return (unsigned char)*left < (unsigned char)*right ? -1 :
           (unsigned char)*left > (unsigned char)*right ? 1 : 0;
}

int strncmp(const char *left, const char *right, size_t length) {
    size_t remaining = length;
    if (length == 0U) return 0;
    if (left == 0 || right == 0) return left == right ? 0 : (left == 0 ? -1 : 1);
    while (remaining != 0U) {
        unsigned char left_value = (unsigned char)*left;
        unsigned char right_value = (unsigned char)*right;
        if (left_value != right_value) return left_value < right_value ? -1 : 1;
        if (left_value == '\0') return 0;
        ++left;
        ++right;
        --remaining;
    }
    return 0;
}

char *strcpy(char *destination, const char *source) {
    char *result = destination;
    if (destination == 0 || source == 0) return destination;
    while ((*destination++ = *source++) != '\0') { }
    return result;
}

char *strncpy(char *destination, const char *source, size_t length) {
    char *result = destination;
    if (destination == 0 || source == 0) return destination;
    while (length != 0U && *source != '\0') {
        *destination++ = *source++;
        --length;
    }
    while (length-- != 0U) *destination++ = '\0';
    return result;
}

char *strchr(const char *text, int value) {
    if (text == 0) return 0;
    while (*text != '\0') {
        if ((unsigned char)*text == (unsigned char)value) return (char *)text;
        ++text;
    }
    return value == 0 ? (char *)text : 0;
}

char *strrchr(const char *text, int value) {
    const char *last = 0;
    if (text == 0) return 0;
    do {
        if ((unsigned char)*text == (unsigned char)value) last = text;
    } while (*text++ != '\0');
    return (char *)last;
}

char *strstr(const char *text, const char *needle) {
    size_t needle_length;
    if (text == 0 || needle == 0) return 0;
    needle_length = strlen(needle);
    if (needle_length == 0U) return (char *)text;
    for (; *text != '\0'; ++text) {
        if (strncmp(text, needle, needle_length) == 0) return (char *)text;
    }
    return 0;
}
