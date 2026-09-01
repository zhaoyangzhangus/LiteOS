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
