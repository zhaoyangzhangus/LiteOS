#ifndef LITEOS_USER_FONT12X24_H
#define LITEOS_USER_FONT12X24_H

#include <stdint.h>

#include <font12x24_data.h>

#define FONT12X24_WIDTH  12U
#define FONT12X24_HEIGHT 24U
#define FONT12X24_FIRST  32U
#define FONT12X24_LAST   126U

static inline const uint8_t *font12x24_glyph(char character)
{
    uint32_t code = (uint8_t)character;

    if (code < FONT12X24_FIRST || code > FONT12X24_LAST) {
        code = (uint32_t)'?';
    }

    return g_font12x24_a8[code - FONT12X24_FIRST];
}

static inline uint32_t font12x24_blend_xrgb8888(uint32_t destination,
                                                 uint32_t source,
                                                 uint8_t alpha)
{
    uint32_t a = alpha;
    uint32_t inverse = 255U - a;
    uint32_t dr = (destination >> 16U) & 0xFFU;
    uint32_t dg = (destination >> 8U) & 0xFFU;
    uint32_t db = destination & 0xFFU;
    uint32_t sr = (source >> 16U) & 0xFFU;
    uint32_t sg = (source >> 8U) & 0xFFU;
    uint32_t sb = source & 0xFFU;
    uint32_t r = (sr * a + dr * inverse + 127U) / 255U;
    uint32_t g = (sg * a + dg * inverse + 127U) / 255U;
    uint32_t b = (sb * a + db * inverse + 127U) / 255U;

    return (destination & 0xFF000000U) |
           (r << 16U) | (g << 8U) | b;
}

static inline void font12x24_draw_glyph(uint32_t *pixels,
                                         uint32_t stride,
                                         uint32_t width,
                                         uint32_t height,
                                         int32_t x,
                                         int32_t y,
                                         char character,
                                         uint32_t color)
{
    const uint8_t *glyph;

    if (pixels == 0) return;
    glyph = font12x24_glyph(character);

    for (uint32_t row = 0U; row < FONT12X24_HEIGHT; ++row) {
        int32_t dy = y + (int32_t)row;
        if (dy < 0 || dy >= (int32_t)height) continue;

        for (uint32_t column = 0U; column < FONT12X24_WIDTH; ++column) {
            int32_t dx = x + (int32_t)column;
            uint8_t alpha;
            uint32_t *destination;

            if (dx < 0 || dx >= (int32_t)width) continue;

            alpha = glyph[row * FONT12X24_WIDTH + column];
            if (alpha == 0U) continue;

            destination =
                &pixels[(uint64_t)(uint32_t)dy * stride + (uint32_t)dx];

            if (alpha == 255U) {
                *destination = color;
            } else {
                *destination =
                    font12x24_blend_xrgb8888(*destination, color, alpha);
            }
        }
    }
}

#endif
