#ifndef LITEOS_BEARSSL_STRING_H
#define LITEOS_BEARSSL_STRING_H

#ifndef _SIZE_T_DEFINED
typedef __SIZE_TYPE__ size_t;
#define _SIZE_T_DEFINED
#endif

void *memcpy(void *destination, const void *source, size_t length);
void *memmove(void *destination, const void *source, size_t length);
void *memset(void *destination, int value, size_t length);
int memcmp(const void *left, const void *right, size_t length);
size_t strlen(const char *text);

#endif
