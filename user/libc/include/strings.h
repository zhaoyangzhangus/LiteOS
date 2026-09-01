#pragma once

#include <string.h>

int ffs(int value);
int ffsl(long value);
int ffsll(long long value);
char *index(const char *text, int value);
char *rindex(const char *text, int value);
void *memmem(const void *buffer, size_t length,
             const void *needle, size_t needle_length);
void *mempcpy(void *destination, const void *source, size_t length);
