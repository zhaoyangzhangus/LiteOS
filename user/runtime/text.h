#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    OS_TEXT_MIN_SIZE = 14U,
    OS_TEXT_DEFAULT_SIZE = 16U,
    OS_TEXT_MAX_SIZE = 50U,
    OS_TEXT_SIZE_STEP = 4U,
};

bool ui_text_init(uint32_t size);
bool ui_text_adjust(int32_t direction);
void ui_text_shutdown(void);
uint32_t ui_text_size(void);
uint32_t ui_text_advance(void);
uint32_t ui_text_line_height(void);
uint32_t ui_text_measure(const char *text);
uint32_t ui_text_measure_range(const char *text, size_t length);
void ui_text_draw(uint32_t *pixels, uint32_t stride, uint32_t width,
                      uint32_t height, int32_t x, int32_t y,
                      const char *text, uint32_t color);
void ui_text_draw_clipped(uint32_t *pixels, uint32_t stride,
                              uint32_t width, uint32_t height, int32_t x,
                              int32_t y, uint32_t right, const char *text,
                              uint32_t color);
void ui_text_draw_rect(uint32_t *pixels, uint32_t stride,
                           uint32_t width, uint32_t height, int32_t x,
                           int32_t y, uint32_t left, uint32_t top,
                           uint32_t right, uint32_t bottom,
                           const char *text, uint32_t color);

#define OS_TEXT_WIDTH  (ui_text_advance())
#define OS_TEXT_HEIGHT (ui_text_line_height())

#ifdef __cplusplus
}
#endif
