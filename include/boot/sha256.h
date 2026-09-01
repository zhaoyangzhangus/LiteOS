#pragma once

#include <uefi.h>

#define LITEOS_SHA256_DIGEST_SIZE 32U

void sha256_compute(const UINT8 *data, UINTN length,
                    UINT8 digest[LITEOS_SHA256_DIGEST_SIZE]);
