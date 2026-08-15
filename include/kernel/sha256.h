#pragma once

#include <kernel/base.h>

#define KERNEL_SHA256_DIGEST_SIZE 32U

void kernel_sha256_compute(const void *data, size_t length,
                           uint8_t digest[KERNEL_SHA256_DIGEST_SIZE]);
bool kernel_sha256_equal(const uint8_t left[KERNEL_SHA256_DIGEST_SIZE],
                         const uint8_t right[KERNEL_SHA256_DIGEST_SIZE]);
bool kernel_sha256_self_test(void);
