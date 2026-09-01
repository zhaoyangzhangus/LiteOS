#pragma once

#include <liteos/libc.h>
#include <stdint.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 2147483647

__attribute__((target("sse"))) double atof(const char *text);
unsigned long strtoul(const char *text, char **end, int base);
long strtol(const char *text, char **end, int base);
long long strtoll(const char *text, char **end, int base);
unsigned long long strtoull(const char *text, char **end, int base);
intmax_t strtoimax(const char *text, char **end, int base);
uintmax_t strtoumax(const char *text, char **end, int base);
double strtod(const char *text, char **end);
float strtof(const char *text, char **end);
long double strtold(const char *text, char **end);
int abs(int value);
long labs(long value);
long long llabs(long long value);
div_t div(int numerator, int denominator);
ldiv_t ldiv(long numerator, long denominator);
lldiv_t lldiv(long long numerator, long long denominator);
void srand(unsigned int seed);
int rand(void);
void qsort(void *base, size_t count, size_t size,
          int (*compare)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t count, size_t size,
              int (*compare)(const void *, const void *));
char *getenv(const char *name);
char *secure_getenv(const char *name);
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
int putenv(char *assignment);
int atexit(void (*function)(void));
int at_quick_exit(void (*function)(void));
void quick_exit(int status) __attribute__((noreturn));
int system(const char *command);
int mkstemp(char *template_name);
char *mktemp(char *template_name);
char *realpath(const char *path, char *resolved);
int getsubopt(char **optionp, char *const *tokens, char **valuep);
