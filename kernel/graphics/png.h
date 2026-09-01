#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <uapi/abi.h>

/* Kernel-owned PNG output storage. The byte order is R, G, B, A. */
typedef struct desktop_png_image {
    uint8_t *storage;
    const uint8_t *pixels;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
} desktop_png_image_t;

bool desktop_png_decode(const uint8_t *encoded, size_t encoded_size,
                        desktop_png_image_t *image);
