#pragma once

#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <libc/mbstate.h>

#ifndef WEOF
#define WEOF ((wint_t)-1)
#endif

#ifndef WCHAR_MIN
#ifdef __WCHAR_MIN__
#define WCHAR_MIN __WCHAR_MIN__
#elif __WCHAR_MAX__ == 0xffffU || __WCHAR_MAX__ == 0xffffffffU
#define WCHAR_MIN 0
#else
#define WCHAR_MIN (-__WCHAR_MAX__ - 1)
#endif
#endif
#ifndef WCHAR_MAX
#define WCHAR_MAX __WCHAR_MAX__
#endif

#ifdef __cplusplus
extern "C" {
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
double wcstod(const wchar_t *text, wchar_t **end);
float wcstof(const wchar_t *text, wchar_t **end);
long double wcstold(const wchar_t *text, wchar_t **end);

long wcstol(const wchar_t *text, wchar_t **end, int base);
unsigned long wcstoul(const wchar_t *text, wchar_t **end, int base);
long long wcstoll(const wchar_t *text, wchar_t **end, int base);
unsigned long long wcstoull(const wchar_t *text, wchar_t **end, int base);

int wcwidth(wchar_t value);
int wcswidth(const wchar_t *text, size_t length);
#ifdef __cplusplus
}
#endif
size_t wcsftime(wchar_t *buffer, size_t capacity, const wchar_t *format,
                const struct tm *value);
int vswprintf(wchar_t *buffer, size_t capacity, const wchar_t *format,
              va_list arguments);
int swprintf(wchar_t *buffer, size_t capacity, const wchar_t *format, ...);
