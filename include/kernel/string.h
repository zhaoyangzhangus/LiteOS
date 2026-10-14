#pragma once
#include <kernel/base.h>

/* Freestanding implementations; no host CRT or SIMD dependency. */
void *memset(void *destination, int value, size_t length);
void *memcpy(void *destination, const void *source, size_t length);
void *memmove(void *destination, const void *source, size_t length);
int memcmp(const void *left, const void *right, size_t length);
