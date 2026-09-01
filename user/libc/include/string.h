#pragma once

#include <liteos/libc.h>

void *memccpy(void *destination, const void *source, int value, size_t length);
void *memchr(const void *buffer, int value, size_t length);
void *memrchr(const void *buffer, int value, size_t length);
char *strcat(char *destination, const char *source);
char *strncat(char *destination, const char *source, size_t length);
char *strchrnul(const char *text, int value);
size_t strxfrm(char *destination, const char *source, size_t length);
int strcoll(const char *left, const char *right);
size_t strspn(const char *text, const char *accept);
size_t strcspn(const char *text, const char *reject);
char *strpbrk(const char *text, const char *accept);
char *strtok(char *text, const char *delimiters);
char *strtok_r(char *text, const char *delimiters, char **state);
char *strdup(const char *text);
char *strndup(const char *text, size_t length);
char *strerror(int error_number);
int strerror_r(int error_number, char *buffer, size_t length);
int strcasecmp(const char *left, const char *right);
int strncasecmp(const char *left, const char *right, size_t length);
char *stpcpy(char *destination, const char *source);
char *stpncpy(char *destination, const char *source, size_t length);
size_t strlcpy(char *destination, const char *source, size_t capacity);
size_t strlcat(char *destination, const char *source, size_t capacity);
char *strcasestr(const char *text, const char *needle);
