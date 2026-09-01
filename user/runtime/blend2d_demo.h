#pragma once

#include <stdbool.h>
#include <stdint.h>

enum {
    LITEOS_BLEND2D_FONT_MIN = 14U,
    LITEOS_BLEND2D_FONT_DEFAULT = 22U,
    LITEOS_BLEND2D_FONT_MAX = 50U,
    LITEOS_BLEND2D_FONT_STEP = 4U,
};

bool liteos_blend2d_draw_demo(uint32_t *pixels, uint32_t stride,
                              uint32_t width, uint32_t height,
                              uint32_t font_size);
