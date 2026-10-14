#pragma once
#include <kernel/base.h>
#include "cursor_resize_data.h"

enum {
    WINDOW_CURSOR_ARROW,
    WINDOW_CURSOR_SIZE_WE,
    WINDOW_CURSOR_SIZE_NS,
    WINDOW_CURSOR_SIZE_NWSE,
    WINDOW_CURSOR_SIZE_NESW,
};

static inline uint32_t window_cursor_hotspot_x(uint32_t shape) {
    if (shape == WINDOW_CURSOR_SIZE_WE) return 11U;
    if (shape == WINDOW_CURSOR_SIZE_NS) return 4U;
    if (shape == WINDOW_CURSOR_SIZE_NWSE || shape == WINDOW_CURSOR_SIZE_NESW) return 8U;
    return 3U;
}
static inline uint32_t window_cursor_hotspot_y(uint32_t shape) {
    if (shape == WINDOW_CURSOR_SIZE_WE) return 4U;
    if (shape == WINDOW_CURSOR_SIZE_NS) return 11U;
    if (shape == WINDOW_CURSOR_SIZE_NWSE || shape == WINDOW_CURSOR_SIZE_NESW) return 8U;
    return 1U;
}

static inline uint32_t window_cursor_pixel(uint32_t shape, uint32_t x, uint32_t y) {
    extern const uint32_t g_linux_cursor_argb[24U * 24U];
    if (x >= 24U || y >= 24U) return 0U;
    if (shape == WINDOW_CURSOR_ARROW) return g_linux_cursor_argb[y * 24U + x];
    if (shape > WINDOW_CURSOR_SIZE_NESW) return 0U;
    uint32_t index = (uint32_t)window_resize_pixels[shape - 1U][y][x];
    index = index >= 'a' ? index - 'a' + 10U : index - '0';
    return window_resize_palette[shape - 1U][index];
}
