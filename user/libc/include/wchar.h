#pragma once

#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef struct mbstate {
    uint32_t codepoint;
    uint32_t minimum;
    uint16_t surrogate;
    unsigned char expected;
} mbstate_t;

#ifndef WEOF
#define WEOF ((wint_t)-1)
#endif

#ifndef WCHAR_MIN
#define WCHAR_MIN 0U
#endif
#ifndef WCHAR_MAX
#define WCHAR_MAX 0xffffU
#endif

size_t wcslen(const wchar_t *text);
size_t wcsnlen(const wchar_t *text, size_t limit);
int wcscmp(const wchar_t *left, const wchar_t *right);
int wcsncmp(const wchar_t *left, const wchar_t *right, size_t length);
wchar_t *wcscpy(wchar_t *destination, const wchar_t *source);
wchar_t *wcsncpy(wchar_t *destination, const wchar_t *source, size_t length);
wchar_t *wcscat(wchar_t *destination, const wchar_t *source);
wchar_t *wcsncat(wchar_t *destination, const wchar_t *source, size_t length);
wchar_t *wcschr(const wchar_t *text, wchar_t value);
wchar_t *wcsrchr(const wchar_t *text, wchar_t value);
wchar_t *wcsstr(const wchar_t *text, const wchar_t *needle);
wchar_t *wcspbrk(const wchar_t *text, const wchar_t *accept);
size_t wcsspn(const wchar_t *text, const wchar_t *accept);
size_t wcscspn(const wchar_t *text, const wchar_t *reject);
wchar_t *wcstok(wchar_t *text, const wchar_t *delimiters,
                wchar_t **state);
size_t wcsxfrm(wchar_t *destination, const wchar_t *source, size_t length);
int wcscoll(const wchar_t *left, const wchar_t *right);
wchar_t *wcsdup(const wchar_t *text);

wchar_t *wmemcpy(wchar_t *destination, const wchar_t *source, size_t count);
wchar_t *wmemmove(wchar_t *destination, const wchar_t *source, size_t count);
wchar_t *wmemset(wchar_t *destination, wchar_t value, size_t count);
int wmemcmp(const wchar_t *left, const wchar_t *right, size_t count);
wchar_t *wmemchr(const wchar_t *buffer, wchar_t value, size_t count);

int mbsinit(const mbstate_t *state);
size_t mbrtowc(wchar_t *wide, const char *multibyte, size_t length,
               mbstate_t *state);
size_t mbrlen(const char *multibyte, size_t length, mbstate_t *state);
size_t wcrtomb(char *multibyte, wchar_t wide, mbstate_t *state);
size_t mbsrtowcs(wchar_t *wide, const char **multibyte, size_t count,
                 mbstate_t *state);
size_t wcsrtombs(char *multibyte, const wchar_t **wide, size_t count,
                 mbstate_t *state);
size_t mbsnrtowcs(wchar_t *wide, const char **multibyte, size_t length,
                  size_t count, mbstate_t *state);
size_t wcsnrtombs(char *multibyte, const wchar_t **wide, size_t length,
                  size_t count, mbstate_t *state);
int mblen(const char *multibyte, size_t length);
int mbtowc(wchar_t *wide, const char *multibyte, size_t length);
int wctomb(char *multibyte, wchar_t wide);
size_t mbstowcs(wchar_t *wide, const char *multibyte, size_t count);
size_t wcstombs(char *multibyte, const wchar_t *wide, size_t count);
wint_t btowc(int value);
int wctob(wint_t value);

long wcstol(const wchar_t *text, wchar_t **end, int base);
unsigned long wcstoul(const wchar_t *text, wchar_t **end, int base);
long long wcstoll(const wchar_t *text, wchar_t **end, int base);
unsigned long long wcstoull(const wchar_t *text, wchar_t **end, int base);

int wcwidth(wchar_t value);
int wcswidth(const wchar_t *text, size_t length);
size_t wcsftime(wchar_t *buffer, size_t capacity, const wchar_t *format,
                const struct tm *value);
