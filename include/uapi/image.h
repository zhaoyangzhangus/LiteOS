#pragma once

#include "abi.h"

/* Keep user image transfers bounded; the kernel decoder uses the same limit. */
#define OS_IMAGE_MAX_ENCODED_BYTES (16ULL * 1024ULL * 1024ULL)
#define OS_IMAGE_MAX_PIXEL_BYTES   (16ULL * 1024ULL * 1024ULL)

enum os_image_pixel_format {
    OS_IMAGE_PIXEL_RGBA8888 = 1U,
};

/* IMAGE_INFO validates a PNG and returns the size of its RGBA output. */
typedef struct os_image_info {
    os_versioned_header_t hdr;
    uint64_t encoded;
    uint64_t encoded_size;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint64_t pixel_bytes;
} os_image_info_t;

/* IMAGE_DECODE copies the decoded RGBA pixels into a caller-owned buffer. */
typedef struct os_image_decode {
    os_versioned_header_t hdr;
    uint64_t encoded;
    uint64_t encoded_size;
    uint64_t pixels;
    uint64_t capacity;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint64_t bytes_written;
} os_image_decode_t;
