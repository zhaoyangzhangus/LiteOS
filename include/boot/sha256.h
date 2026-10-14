#pragma once

#include <uefi.h>

#define OS_SHA256_DIGEST_SIZE 32U

void sha256_compute(const UINT8 *data, UINTN length,
                    UINT8 digest[OS_SHA256_DIGEST_SIZE]);
