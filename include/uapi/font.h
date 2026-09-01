#pragma once

#include "abi.h"

#define OS_FONT_FIRST           0x20U
#define OS_FONT_LAST            0x7EU
#define OS_FONT_GLYPH_COUNT     (OS_FONT_LAST - OS_FONT_FIRST + 1U)
#define OS_FONT_MAX_WIDTH       64U
#define OS_FONT_MAX_HEIGHT      64U

/*
 * The kernel owns the TrueType source and returns a complete ASCII A8 cache
 * for the requested runtime size.  `pixels` points to writable user memory;
 * glyphs are laid out consecutively with one byte per pixel and no padding.
 */
typedef struct os_font_cache_request {
    os_versioned_header_t hdr;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t glyph_count;
    uint64_t pixels;
    uint64_t capacity;
    uint64_t bytes_written;
} os_font_cache_request_t;
