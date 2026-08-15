#ifndef LITEOS_SHA256_H
#define LITEOS_SHA256_H

#include "uefi.h"

VOID sha256_compute(const UINT8 *data, UINTN length, UINT8 digest[32]);
BOOLEAN sha256_parse_hex(const CHAR8 *text, UINT8 digest[32]);
BOOLEAN sha256_equal(const UINT8 a[32], const UINT8 b[32]);

#endif
