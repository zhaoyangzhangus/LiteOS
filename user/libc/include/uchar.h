#pragma once

#include <stddef.h>
#include <wchar.h>

#ifndef __cplusplus
typedef __CHAR16_TYPE__ char16_t;
typedef __CHAR32_TYPE__ char32_t;
#endif

#ifndef __STDC_UTF_16__
#define __STDC_UTF_16__ 1
#endif
#ifndef __STDC_UTF_32__
#define __STDC_UTF_32__ 1
#endif

size_t mbrtoc16(char16_t *output, const char *multibyte, size_t length,
                 mbstate_t *state);
size_t c16rtomb(char *multibyte, char16_t value, mbstate_t *state);
size_t mbrtoc32(char32_t *output, const char *multibyte, size_t length,
                 mbstate_t *state);
size_t c32rtomb(char *multibyte, char32_t value, mbstate_t *state);
