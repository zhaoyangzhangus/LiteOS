#ifndef OS_KERNEL_DESKTOP_ICON_H
#define OS_KERNEL_DESKTOP_ICON_H

#include <stdint.h>

/* Area-filter straight RGBA artwork in premultiplied space, then return
 * straight ARGB for desktop blending. Transparent RGB never leaks a fringe. */
static inline uint32_t desktop_icon_area_pixel(const uint8_t *pixels,
    uint32_t stride, uint32_t left, uint32_t top,
    uint32_t source_width, uint32_t source_height,
    uint32_t width, uint32_t height, uint32_t column, uint32_t row) {
    if (!pixels || !source_width || !source_height || !width || !height ||
        column >= width || row >= height) return 0U;
    uint64_t x0 = (uint64_t)column * source_width;
    uint64_t x1 = (uint64_t)(column + 1U) * source_width;
    uint64_t y0 = (uint64_t)row * source_height;
    uint64_t y1 = (uint64_t)(row + 1U) * source_height;
    uint64_t alpha = 0U, red = 0U, green = 0U, blue = 0U;
    for (uint32_t sy = (uint32_t)(y0 / height); sy <= (y1 - 1U) / height; ++sy) {
        uint64_t a = (uint64_t)sy * height, b = a + height;
        uint64_t wy = (b < y1 ? b : y1) - (a > y0 ? a : y0);
        for (uint32_t sx = (uint32_t)(x0 / width); sx <= (x1 - 1U) / width; ++sx) {
            a = (uint64_t)sx * width; b = a + width;
            uint64_t wx = (b < x1 ? b : x1) - (a > x0 ? a : x0);
            const uint8_t *p = pixels + (uint64_t)(top + sy) * stride +
                               (uint64_t)(left + sx) * 4U;
            uint64_t weight = wx * wy * p[3];
            alpha += weight;
            red += weight * p[0]; green += weight * p[1]; blue += weight * p[2];
        }
    }
    if (!alpha) return 0U;
    uint64_t area = (uint64_t)source_width * source_height;
    return (uint32_t)((alpha + area / 2U) / area) << 24U |
           (uint32_t)((red + alpha / 2U) / alpha) << 16U |
           (uint32_t)((green + alpha / 2U) / alpha) << 8U |
           (uint32_t)((blue + alpha / 2U) / alpha);
}

/* Vector coordinates are in 1/256 units on a 48-unit design canvas. */
static inline uint32_t desktop_icon_vector_sample(uint32_t kind, int32_t x, int32_t y) {
    if (kind == 0U) {
        int32_t dx = x - 24 * 256, dy = y - 24 * 256;
        int64_t radius = (int64_t)dx * dx + (int64_t)dy * dy;
        if (radius > (int64_t)(22 * 256) * (22 * 256)) return 0U;
        if (radius <= (int64_t)(9 * 256) * (9 * 256)) return 0xFF4285F4U;
        if (radius <= (int64_t)(11 * 256) * (11 * 256)) return 0xFFFFFFFFU;
        if (dy < -9 * 256 || (dx > 0 && dy * 1000 < dx * -577)) return 0xFFEA4335U;
        if (dx < 0 && dy * 1000 < dx * -577) return 0xFF34A853U;
        return 0xFFFBBC05U;
    }
    if (x < 4 * 256 || y < 4 * 256 || x >= 44 * 256 || y >= 44 * 256) return 0U;
    int32_t cx = x < 13 * 256 ? 13 * 256 : x > 35 * 256 ? 35 * 256 : x;
    int32_t cy = y < 13 * 256 ? 13 * 256 : y > 35 * 256 ? 35 * 256 : y;
    int32_t dx = x - cx, dy = y - cy;
    if ((int64_t)dx * dx + (int64_t)dy * dy > (int64_t)(9 * 256) * (9 * 256)) return 0U;
    if (y >= 34 * 256 && y < 36 * 256 && x >= 10 * 256 && x < 38 * 256) return 0xFF8498B0U;
    if (x >= 12 * 256 && x < 18 * 256 && y >= 25 * 256 && y < 34 * 256) return 0xFF8AB4F8U;
    if (x >= 22 * 256 && x < 28 * 256 && y >= 19 * 256 && y < 34 * 256) return 0xFF6D9FF4U;
    if (x >= 32 * 256 && x < 38 * 256 && y >= 12 * 256 && y < 34 * 256) return 0xFF6DD6A4U;
    uint32_t shade = (uint32_t)(44 * 256 - y) / 512U;
    return 0xFF1E293BU + (shade << 16U) + (shade << 8U) + shade;
}

static inline uint32_t desktop_icon_vector_pixel(uint32_t kind,
    uint32_t size, uint32_t column, uint32_t row) {
    if (!size || column >= size || row >= size) return 0U;
    uint32_t alpha = 0U, red = 0U, green = 0U, blue = 0U;
    for (uint32_t sy = 0U; sy < 4U; ++sy) for (uint32_t sx = 0U; sx < 4U; ++sx) {
        int32_t x = (int32_t)(((uint64_t)column * 8U + sx * 2U + 1U) * 48U * 256U / (size * 8U));
        int32_t y = (int32_t)(((uint64_t)row * 8U + sy * 2U + 1U) * 48U * 256U / (size * 8U));
        uint32_t p = desktop_icon_vector_sample(kind, x, y);
        uint32_t a = p >> 24U;
        alpha += a; red += ((p >> 16U) & 255U) * a;
        green += ((p >> 8U) & 255U) * a; blue += (p & 255U) * a;
    }
    if (!alpha) return 0U;
    return ((alpha + 8U) / 16U) << 24U |
           ((red + alpha / 2U) / alpha) << 16U |
           ((green + alpha / 2U) / alpha) << 8U | ((blue + alpha / 2U) / alpha);
}
#endif
