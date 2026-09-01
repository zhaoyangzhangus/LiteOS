#pragma once

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

/* Internal UTF-8 primitives shared by the wide-character and C11 Unicode
 * conversion front ends.  They operate on Unicode scalar values; callers
 * choose the destination character representation. */
void __libc_mbstate_reset(mbstate_t *state);
int __libc_unicode_scalar_valid(uint32_t value);
size_t __libc_utf8_decode(uint32_t *value, const char *multibyte,
                          size_t length, mbstate_t *state);
size_t __libc_utf8_encode(char *multibyte, uint32_t value,
                          mbstate_t *state);
