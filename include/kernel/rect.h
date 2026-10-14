#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Half-open rectangle: [x0, x1) x [y0, y1). */
typedef struct Rect {
    int32_t x0;
    int32_t y0;
    int32_t x1;
    int32_t y1;
} Rect;

static inline bool rect_is_empty(Rect rect) {
    return rect.x0 >= rect.x1 || rect.y0 >= rect.y1;
}

static inline bool rect_intersects(Rect a, Rect b) {
    return a.x0 < b.x1 && b.x0 < a.x1 &&
           a.y0 < b.y1 && b.y0 < a.y1;
}

/* Clip wide coordinates before narrowing to Rect. Failure leaves output
 * unchanged; display extents must fit Rect's signed coordinate domain. */
static inline bool rect_clip_to_extent(int64_t left, int64_t top,
    int64_t right, int64_t bottom, uint32_t width, uint32_t height, Rect *out) {
    if (!out || width > INT32_MAX || height > INT32_MAX) return false;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > (int64_t)width) right = width;
    if (bottom > (int64_t)height) bottom = height;
    if (left >= right || top >= bottom) return false;
    *out = (Rect){(int32_t)left, (int32_t)top, (int32_t)right, (int32_t)bottom};
    return true;
}
