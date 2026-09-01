#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    LITEOS_TEXT_MIN_SIZE = 14U,
    LITEOS_TEXT_DEFAULT_SIZE = 22U,
    LITEOS_TEXT_MAX_SIZE = 50U,
    LITEOS_TEXT_SIZE_STEP = 4U,
};

bool liteos_text_init(uint32_t size);
bool liteos_text_adjust(int32_t direction);
void liteos_text_shutdown(void);
uint32_t liteos_text_size(void);
uint32_t liteos_text_advance(void);
uint32_t liteos_text_line_height(void);
uint32_t liteos_text_measure(const char *text);
uint32_t liteos_text_measure_range(const char *text, size_t length);
void liteos_text_draw(uint32_t *pixels, uint32_t stride, uint32_t width,
                      uint32_t height, int32_t x, int32_t y,
                      const char *text, uint32_t color);
void liteos_text_draw_clipped(uint32_t *pixels, uint32_t stride,
                              uint32_t width, uint32_t height, int32_t x,
                              int32_t y, uint32_t right, const char *text,
                              uint32_t color);

#define LITEOS_TEXT_WIDTH  (liteos_text_advance())
#define LITEOS_TEXT_HEIGHT (liteos_text_line_height())

#ifdef __cplusplus
}
#endif
