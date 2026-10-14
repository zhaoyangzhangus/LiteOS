#ifndef OS_MESSAGE_BOX_H
#define OS_MESSAGE_BOX_H

#include <uapi/window.h>
#include "../client_chrome.h"

/* Non-blocking, application-modal dialog on the owner's surface. The owner
 * draws this last, routes input here first and invalidates its whole surface
 * on show/dismiss. No second event loop, window mapping or font lifetime. */
enum ui_message_buttons {
    OS_MESSAGE_OK, OS_MESSAGE_OK_CANCEL,
    OS_MESSAGE_YES_NO, OS_MESSAGE_SAVE_DISCARD_CANCEL
};
enum ui_message_result {
    OS_MESSAGE_NONE, OS_MESSAGE_ACCEPT, OS_MESSAGE_CANCEL,
    OS_MESSAGE_YES, OS_MESSAGE_NO,
    OS_MESSAGE_SAVE, OS_MESSAGE_DISCARD
};
typedef struct ui_message_box {
    bool active;
    enum ui_message_buttons buttons;
    uint32_t focus;
    int32_t pressed;
    char title[64];
    char message[512];
} ui_message_box_t;
typedef struct ui_message_layout {
    uint32_t x, y, width, height, button_y, button_width, button_height;
    uint32_t count, gap, inset;
} ui_message_layout_t;

static inline void ui_message_copy(char *out, uint32_t capacity,
                                       const char *text) {
    uint32_t i = 0U;
    if (text) while (i + 1U < capacity && text[i]) { out[i] = text[i]; ++i; }
    out[i] = '\0';
}
static inline uint32_t ui_message_count(enum ui_message_buttons buttons) {
    return buttons == OS_MESSAGE_OK ? 1U :
        buttons == OS_MESSAGE_SAVE_DISCARD_CANCEL ? 3U : 2U;
}
static inline void ui_message_show(ui_message_box_t *box,
    const char *title, const char *message, enum ui_message_buttons buttons) {
    if (!box) return;
    if ((uint32_t)buttons > OS_MESSAGE_SAVE_DISCARD_CANCEL) buttons = OS_MESSAGE_OK;
    box->buttons = buttons;
    box->focus = ui_message_count(buttons) - 1U;
    box->pressed = -1;
    ui_message_copy(box->title, sizeof(box->title), title);
    ui_message_copy(box->message, sizeof(box->message), message);
    box->active = true;
}
static inline const char *ui_message_label(enum ui_message_buttons b,
                                               uint32_t i) {
    if (b == OS_MESSAGE_SAVE_DISCARD_CANCEL)
        return i == 0U ? "Save" : i == 1U ? "Discard" : "Cancel";
    if (b == OS_MESSAGE_YES_NO) return i == 0U ? "Yes" : "No";
    return i == 0U ? "OK" : "Cancel";
}
static inline enum ui_message_result ui_message_choice(
    enum ui_message_buttons b, uint32_t i) {
    if (b == OS_MESSAGE_SAVE_DISCARD_CANCEL)
        return i == 0U ? OS_MESSAGE_SAVE : i == 1U ? OS_MESSAGE_DISCARD : OS_MESSAGE_CANCEL;
    if (b == OS_MESSAGE_YES_NO) return i == 0U ? OS_MESSAGE_YES : OS_MESSAGE_NO;
    return i == 0U ? OS_MESSAGE_ACCEPT : OS_MESSAGE_CANCEL;
}
static inline ui_message_layout_t ui_message_layout(
    const ui_message_box_t *box, uint32_t width, uint32_t height) {
    ui_message_layout_t r = {0};
    uint32_t line = ui_text_line_height();
    r.count = ui_message_count(box->buttons);
    r.width = 540U + (line > 24U ? (line - 24U) * 10U : 0U);
    if (r.width > width) r.width = width;
    r.height = line * 8U + 56U;
    if (r.height > height) r.height = height;
    r.x = (width - r.width) / 2U; r.y = (height - r.height) / 2U;
    r.inset = r.width >= 96U ? 16U : 0U;
    r.gap = r.width >= 96U ? 8U : 0U;
    r.button_width = (r.width - 2U * r.inset - (r.count - 1U) * r.gap) / r.count;
    r.button_height = line + 16U;
    if (r.button_height > r.height) r.button_height = r.height;
    uint32_t margin = r.height - r.button_height >= 16U ? 16U : 0U;
    r.button_y = r.y + r.height - r.button_height - margin;
    return r;
}
static inline int32_t ui_message_hit(ui_message_layout_t r,
                                        int32_t x, int32_t y) {
    if (x < 0 || y < 0 || (uint32_t)y < r.button_y ||
        (uint32_t)y - r.button_y >= r.button_height) return -1;
    for (uint32_t i = 0U; i < r.count; ++i) {
        uint32_t left = r.x + r.inset + i * (r.button_width + r.gap);
        if ((uint32_t)x >= left && (uint32_t)x - left < r.button_width) return (int32_t)i;
    }
    return -1;
}
static inline enum ui_message_result ui_message_event(
    ui_message_box_t *box, uint32_t width, uint32_t height,
    const os_window_event_t *event) {
    if (!box || !box->active || !event) return OS_MESSAGE_NONE;
    enum ui_message_result result = OS_MESSAGE_NONE;
    if (event->type == OS_WINDOW_EVENT_CLOSE_REQUEST) result = OS_MESSAGE_CANCEL;
    if (event->type == OS_WINDOW_EVENT_INPUT) {
        const os_input_event_t *input = &event->input;
        if (input->type == OS_INPUT_EVENT_KEY && input->value == OS_INPUT_VALUE_PRESS) {
            uint32_t count = ui_message_count(box->buttons);
            if (input->code == 0x29U) result = OS_MESSAGE_CANCEL;
            else if (input->code == 0x28U || input->code == 0x58U || input->code == 0x2CU)
                result = ui_message_choice(box->buttons, box->focus);
            else if (input->code == 0x2BU || input->code == 0x4FU)
                box->focus = (box->focus + 1U) % count;
            else if (input->code == 0x50U) box->focus = (box->focus + count - 1U) % count;
        } else if (input->type == OS_INPUT_EVENT_BUTTON && input->code == OS_INPUT_BUTTON_LEFT) {
            ui_message_layout_t layout = ui_message_layout(box, width, height);
            int32_t hit = ui_message_hit(layout,
                                            event->pointer_x, event->pointer_y);
            if (layout.width >= 64U && layout.height >= 48U &&
                event->pointer_x >= (int32_t)(layout.x + layout.width - 40U) &&
                event->pointer_x < (int32_t)(layout.x + layout.width - 8U) &&
                event->pointer_y >= (int32_t)(layout.y + 8U) &&
                event->pointer_y < (int32_t)(layout.y + 40U)) hit = -2;
            if (input->value == OS_INPUT_VALUE_PRESS) {
                box->pressed = hit;
                if (hit >= 0) box->focus = (uint32_t)hit;
            } else if (input->value == OS_INPUT_VALUE_RELEASE) {
                if (hit == -2 && box->pressed == -2) result = OS_MESSAGE_CANCEL;
                else if (hit >= 0 && hit == box->pressed) result = ui_message_choice(box->buttons, (uint32_t)hit);
                box->pressed = -1;
            }
        }
    }
    if (result != OS_MESSAGE_NONE) { box->active = false; box->pressed = -1; }
    return result;
}

static inline void ui_message_draw(const ui_message_box_t *box,
    uint32_t *pixels, uint32_t stride, uint32_t width, uint32_t height) {
    if (!box || !box->active || !pixels || stride < width) return;
    ui_message_layout_t r = ui_message_layout(box, width, height);
    uint32_t line = ui_text_line_height();
    /* Redraw the owner first; never blend the dim layer onto a prior dialog. */
    for (uint32_t y = 0U; y < height; ++y)
        for (uint32_t x = 0U; x < width; ++x)
            pixels[(uint64_t)y * stride + x] = user_client_chrome_blend_xrgb8888(
                pixels[(uint64_t)y * stride + x], 0U, 60U);
    user_client_chrome_fill_rect(pixels, stride, width, height, r.x, r.y,
                                 r.width, r.height, OS_THEME_BORDER);
    if (r.width > 2U && r.height > 2U)
        user_client_chrome_fill_rect(pixels, stride, width, height, r.x + 1U, r.y + 1U,
                                     r.width - 2U, r.height - 2U, OS_THEME_SURFACE);
    uint32_t left = r.x + r.inset, right = r.x + r.width - r.inset;
    uint32_t top = r.y + 16U;
    ui_text_draw_rect(pixels, stride, width, height, (int32_t)left, (int32_t)top,
                          left, r.y, right > left + 32U ? right - 32U : left,
                          r.button_y, box->title, OS_THEME_TEXT);
    if (r.width >= 64U && r.height >= 48U)
        for (uint32_t y = 0U; y < 16U; ++y)
            for (uint32_t x = 0U; x < 16U; ++x)
                user_client_chrome_pixel(pixels, stride, width, height,
                    r.x + r.width - 32U + x, r.y + 16U + y, OS_THEME_MUTED,
                    (uint8_t)(user_client_chrome_nibble(user_client_close_alpha[y][x]) * 17U));
    top += line + 16U;
    const char *text = box->message;
    /* Wrap at spaces/newlines, with UTF-8-safe hard wrapping for long words. */
    while (*text && line && top < r.button_y && r.button_y - top >= line) {
        size_t n = 0U, word = 0U;
        while (text[n] && text[n] != '\n') {
            size_t next = n + 1U;
            while (((unsigned char)text[next] & 0xC0U) == 0x80U) ++next;
            if (ui_text_measure_range(text, next) > right - left) break;
            if (text[n] == ' ') word = n;
            n = next;
        }
        if (text[n] && text[n] != '\n' && word) n = word;
        if (!n && *text != '\n') break;
        char row[512];
        for (size_t i = 0U; i < n; ++i) row[i] = text[i];
        row[n] = '\0';
        ui_text_draw_rect(pixels, stride, width, height, (int32_t)left, (int32_t)top,
                              left, r.y, right, r.button_y, row, OS_THEME_TEXT);
        text += n;
        if (*text == '\n') ++text;
        while (*text == ' ') ++text;
        top += line;
    }
    for (uint32_t i = 0U; i < r.count; ++i) {
        uint32_t x = left + i * (r.button_width + r.gap);
        uint32_t color = i == box->focus ? OS_THEME_ACCENT : OS_THEME_FIELD;
        user_client_chrome_fill_rect(pixels, stride, width, height, x, r.button_y,
                                     r.button_width, r.button_height, color);
        const char *label = ui_message_label(box->buttons, i);
        uint32_t size = ui_text_measure(label);
        uint32_t tx = x + (r.button_width > size ? (r.button_width - size) / 2U : 0U);
        uint32_t ty = r.button_y + (r.button_height > line ? (r.button_height - line) / 2U : 0U);
        ui_text_draw_rect(pixels, stride, width, height, (int32_t)tx, (int32_t)ty,
            x, r.button_y, x + r.button_width, r.button_y + r.button_height,
            label, i == box->focus ? 0xFFFFFFFFU : OS_THEME_TEXT);
    }
}
#endif
