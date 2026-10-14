#pragma once

#include <stdbool.h>
#include <stdint.h>

enum {
    OS_BLEND2D_FONT_MIN = 14U,
    OS_BLEND2D_FONT_DEFAULT = 22U,
    OS_BLEND2D_FONT_MAX = 50U,
    OS_BLEND2D_FONT_STEP = 4U,
};

bool blend2d_draw_demo(uint32_t *pixels, uint32_t stride,
                              uint32_t width, uint32_t height,
                              uint32_t font_size);
