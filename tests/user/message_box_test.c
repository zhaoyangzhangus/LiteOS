#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../../user/runtime/message_box.h"

static uint32_t font = 20U;
uint32_t ui_text_line_height(void) { return font; }
uint32_t ui_text_measure_range(const char *text, size_t n) { (void)text; return (uint32_t)n * (font / 2U); }
uint32_t ui_text_measure(const char *text) { return ui_text_measure_range(text, strlen(text)); }
void ui_text_draw_rect(uint32_t *p, uint32_t stride, uint32_t width, uint32_t height,
    int32_t x, int32_t y, uint32_t left, uint32_t top, uint32_t right, uint32_t bottom,
    const char *text, uint32_t color) {
    (void)text;
    if (x >= 0 && y >= 0 && (uint32_t)x >= left && (uint32_t)y >= top &&
        (uint32_t)x < right && (uint32_t)y < bottom && (uint32_t)x < width && (uint32_t)y < height)
        p[(uint64_t)y * stride + x] = color;
}
static os_window_event_t key(uint32_t code, int value) {
    os_window_event_t e = {0}; e.input.type = OS_INPUT_EVENT_KEY; e.input.code = code; e.input.value = value; return e;
}
static os_window_event_t click(int x, int y, int value) {
    os_window_event_t e = {0}; e.input.type = OS_INPUT_EVENT_BUTTON; e.input.code = OS_INPUT_BUTTON_LEFT;
    e.input.value = value; e.pointer_x = x; e.pointer_y = y; return e;
}
int main(void) {
    ui_message_box_t box = {0};
    for (unsigned mode = 0; mode <= 3; ++mode) {
        for (unsigned i = 0; i < ui_message_count(mode); ++i) {
            ui_message_show(&box, "Title", "Some message", mode);
            ui_message_layout_t r = ui_message_layout(&box, 800, 600);
            int x = r.x + r.inset + i * (r.button_width + r.gap) + 2, y = r.button_y + 2;
            os_window_event_t e = click(x, y, OS_INPUT_VALUE_RELEASE);
            assert(ui_message_event(&box, 800, 600, &e) == OS_MESSAGE_NONE);
            e = click(x, y, OS_INPUT_VALUE_PRESS);
            assert(ui_message_event(&box, 800, 600, &e) == OS_MESSAGE_NONE && box.active);
            e = click(-1, -1, OS_INPUT_VALUE_RELEASE);
            assert(ui_message_event(&box, 800, 600, &e) == OS_MESSAGE_NONE && box.active);
            e = click(x, y, OS_INPUT_VALUE_PRESS); ui_message_event(&box, 800, 600, &e);
            e.input.value = OS_INPUT_VALUE_RELEASE;
            assert(ui_message_event(&box, 800, 600, &e) == ui_message_choice(mode, i) && !box.active);
        }
        ui_message_show(&box, "Title", "Message", mode);
        os_window_event_t e = key(0x28, OS_INPUT_VALUE_RELEASE);
        assert(ui_message_event(&box, 800, 600, &e) == OS_MESSAGE_NONE && box.active);
        e = key(0x2B, OS_INPUT_VALUE_PRESS); ui_message_event(&box, 800, 600, &e); assert(box.focus == 0);
        e = key(0x50, OS_INPUT_VALUE_PRESS); ui_message_event(&box, 800, 600, &e);
        assert(box.focus == ui_message_count(mode) - 1);
        e = key(0x29, OS_INPUT_VALUE_PRESS);
        assert(ui_message_event(&box, 800, 600, &e) == OS_MESSAGE_CANCEL && !box.active);
        ui_message_show(&box, "Title", "Message", mode);
        e.type = OS_WINDOW_EVENT_CLOSE_REQUEST;
        assert(ui_message_event(&box, 800, 600, &e) == OS_MESSAGE_CANCEL);
        ui_message_show(&box, "Title", "Message", mode);
        ui_message_layout_t r = ui_message_layout(&box, 800, 600);
        e = click(r.x + r.width - 24, r.y + 24, OS_INPUT_VALUE_PRESS);
        ui_message_event(&box, 800, 600, &e); e.input.value = OS_INPUT_VALUE_RELEASE;
        assert(ui_message_event(&box, 800, 600, &e) == OS_MESSAGE_CANCEL);
    }
    char long_text[1024]; memset(long_text, 'x', 1023); long_text[1023] = 0;
    ui_message_show(&box, long_text, long_text, OS_MESSAGE_SAVE_DISCARD_CANCEL);
    assert(strlen(box.title) == 63 && strlen(box.message) == 511);
    for (unsigned size = 0; size < 800; ++size) {
        uint32_t w = size, h = size / 2U, stride = w + 3U;
        font = 16 + size % 49;
        size_t n = (size_t)stride * h;
        uint32_t *pixels = malloc((n + 2) * sizeof(*pixels)); assert(pixels);
        for (size_t i = 0; i < n + 2; ++i) pixels[i] = 0x12345678;
        ui_message_layout_t r = ui_message_layout(&box, w, h);
        assert(r.x + r.width <= w && r.y + r.height <= h);
        ui_message_draw(&box, pixels + 1, stride, w, h);
        assert(pixels[0] == 0x12345678 && pixels[n + 1] == 0x12345678);
        for (unsigned y = 0; y < h; ++y) for (unsigned x = w; x < stride; ++x)
            assert(pixels[1 + (size_t)y * stride + x] == 0x12345678);
        free(pixels);
    }
    puts("Message box: all button modes, press/release, outside cancellation, Tab/arrows/Escape, close, bounded text and 800 font/viewport layouts passed");
}
