#include "liteos/libc.h"

#include <strings.h>

static int first_set_bit(unsigned long long value) {
    int position = 1;
    if (value == 0U) return 0;
    while ((value & 1U) == 0U) {
        value >>= 1U;
        ++position;
    }
    return position;
}

int ffs(int value) {
    return first_set_bit((unsigned int)value);
}

int ffsl(long value) {
    return first_set_bit((unsigned long)value);
}

int ffsll(long long value) {
    return first_set_bit((unsigned long long)value);
}

char *index(const char *text, int value) {
    return strchr(text, value);
}

char *rindex(const char *text, int value) {
    return strrchr(text, value);
}

void *memmem(const void *buffer, size_t length,
             const void *needle, size_t needle_length) {
    const unsigned char *bytes = (const unsigned char *)buffer;
    const unsigned char *pattern = (const unsigned char *)needle;
    size_t index;
    if (needle_length == 0U) return (void *)buffer;
    if (buffer == 0 || needle == 0 || needle_length > length) return 0;
    for (index = 0U; index + needle_length <= length; ++index) {
        if (memcmp(bytes + index, pattern, needle_length) == 0) {
            return (void *)(bytes + index);
        }
    }
    return 0;
}

void *mempcpy(void *destination, const void *source, size_t length) {
    memcpy(destination, source, length);
    return (unsigned char *)destination + length;
}
