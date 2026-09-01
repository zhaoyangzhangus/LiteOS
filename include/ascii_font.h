#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <uapi/abi.h>

#define ASCII_FONT_FIRST       0x20U
#define ASCII_FONT_LAST        0x7EU
#define ASCII_FONT_DEFAULT_WIDTH  8U
#define ASCII_FONT_DEFAULT_HEIGHT 16U

/* The kernel owns the font file and exposes an A8 glyph cache.  Set the
 * target size before drawing; the next lookup then returns one byte per
 * pixel at that size. */
bool ascii_font_load(void);
bool ascii_font_set_size(uint32_t width, uint32_t height);
const uint8_t *ascii_font_glyph(uint8_t character);
uint32_t ascii_font_width(void);
uint32_t ascii_font_height(void);
bool ascii_font_copy_cache(uint32_t width, uint32_t height,
                           uint8_t *destination, size_t capacity);
