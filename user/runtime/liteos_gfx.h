#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The graphics ABI accepts only a caller-owned XRGB8888 buffer. Blend2D owns
 * the rasterization state while the caller retains ownership of the pixels. */
void liteos_gfx_clear(uint32_t *pixels, uint32_t stride, uint32_t width,
                      uint32_t height, uint32_t color);
void liteos_gfx_fill_rect(uint32_t *pixels, uint32_t stride, uint32_t width,
                          uint32_t height, int32_t x, int32_t y,
                          uint32_t rect_width, uint32_t rect_height,
                          uint32_t color);
void liteos_gfx_gradient_rect(uint32_t *pixels, uint32_t stride, uint32_t width,
                              uint32_t height, int32_t x, int32_t y,
                              uint32_t rect_width, uint32_t rect_height,
                              uint32_t top_color, uint32_t bottom_color);
void liteos_gfx_frame(uint32_t *pixels, uint32_t stride, uint32_t width,
                      uint32_t height, uint32_t thickness, uint32_t color);

#ifdef __cplusplus
}
#endif
