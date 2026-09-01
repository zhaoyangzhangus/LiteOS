#include <kernel/display.h>
#include <kernel/sched.h>
#include "internal.h"

/* REFACTOR_P7J_PRESENT_CURSOR_OWNER: scanout-side cursor overlay. */

/* The retained compositor scene stays cursor-free; presentation owns the
 * small direct overlay and its restoration of the previous pointer image. */
void window_present_cursor_reset_locked(void) {
    g_window_server.presented_pointer_x = 0U;
    g_window_server.presented_pointer_y = 0U;
    g_window_server.presented_pointer_valid = false;
}

bool window_present_self_test(void) {
    return WINDOW_CURSOR_WIDTH != 0U &&
           WINDOW_CURSOR_HEIGHT != 0U &&
           WINDOW_CURSOR_HOTSPOT_X < WINDOW_CURSOR_WIDTH &&
           WINDOW_CURSOR_HOTSPOT_Y < WINDOW_CURSOR_HEIGHT &&
           g_linux_cursor_argb[0] == 0U;
}

static uint32_t cursor_blend_xrgb(uint32_t background, uint32_t cursor) {
    uint32_t alpha = cursor >> 24;
    uint32_t color = cursor & 0x00FFFFFFU;
    uint32_t inverse;

    if (alpha == 0U) return background;
    if (alpha >= 255U) return color;

    inverse = 255U - alpha;
    return (((((background >> 16) & 0xFFU) * inverse +
              ((color >> 16) & 0xFFU) * alpha + 127U) / 255U) << 16) |
           (((((background >> 8) & 0xFFU) * inverse +
              ((color >> 8) & 0xFFU) * alpha + 127U) / 255U) << 8) |
           (((background & 0xFFU) * inverse +
             (color & 0xFFU) * alpha + 127U) / 255U);
}

static void cursor_publish_row(int64_t origin_x, int64_t y,
                               uint32_t cursor_row, bool draw_cursor) {
    uint32_t scanline[WINDOW_CURSOR_WIDTH];
    int64_t left = origin_x < 0 ? 0 : origin_x;
    int64_t right = origin_x + (int64_t)WINDOW_CURSOR_WIDTH;
    uint32_t pixels;

    if (y < 0 || y >= (int64_t)g_window_server.display_height) return;
    if (right > (int64_t)g_window_server.display_width) {
        right = g_window_server.display_width;
    }
    if (left >= right) return;

    pixels = (uint32_t)(right - left);
    for (uint32_t column = 0U; column < pixels; ++column) {
        uint32_t x = (uint32_t)left + column;
        uint64_t offset = (uint64_t)(uint32_t)y *
                          g_window_server.display_stride + x;
        uint32_t background =
            g_window_server.composite_framebuffer[offset];

        if (draw_cursor) {
            uint32_t source_column =
                (uint32_t)((int64_t)x - origin_x);
            uint32_t cursor = g_linux_cursor_argb[
                cursor_row * WINDOW_CURSOR_WIDTH + source_column];
            scanline[column] = cursor_blend_xrgb(background, cursor);
        } else {
            scanline[column] = background;
        }
    }

    display_core_publish_xrgb8888_span(
        g_window_server.framebuffer +
            (uint64_t)(uint32_t)y * g_window_server.display_stride +
            (uint32_t)left,
        scanline,
        pixels);
}

void compositor_present_cursor_direct(bool force) {
    int64_t left;
    int64_t top;
    int64_t right;
    int64_t bottom;

    if (!window_present_cursor_overlay(force,
                                       &left,
                                       &top,
                                       &right,
                                       &bottom)) {
        return;
    }

    compositor_qemu_repair_mark_front_cursor(left, top, right, bottom);
}

bool window_present_cursor_overlay(bool force,
                                   int64_t *out_left,
                                   int64_t *out_top,
                                   int64_t *out_right,
                                   int64_t *out_bottom) {
    uint32_t pointer_x;
    uint32_t pointer_y;
    int64_t old_origin_x;
    int64_t old_origin_y;
    int64_t new_origin_x;
    int64_t new_origin_y;

    if (g_window_server.framebuffer == 0 ||
        g_window_server.composite_framebuffer == 0 ||
        g_window_server.composite_framebuffer ==
            g_window_server.framebuffer ||
        g_window_server.display_width == 0U ||
        g_window_server.display_height == 0U) {
        return false;
    }

    pointer_x = g_window_server.pointer_x;
    pointer_y = g_window_server.pointer_y;

    if (!force &&
        g_window_server.presented_pointer_valid &&
        g_window_server.presented_pointer_x == pointer_x &&
        g_window_server.presented_pointer_y == pointer_y) {
        return false;
    }

    sched_preempt_disable();

    if (g_window_server.presented_pointer_valid) {
        old_origin_x =
            (int64_t)g_window_server.presented_pointer_x -
            WINDOW_CURSOR_HOTSPOT_X;
        old_origin_y =
            (int64_t)g_window_server.presented_pointer_y -
            WINDOW_CURSOR_HOTSPOT_Y;

        for (uint32_t row = 0U; row < WINDOW_CURSOR_HEIGHT; ++row) {
            cursor_publish_row(old_origin_x,
                               old_origin_y + (int64_t)row,
                               row, false);
        }
    }

    new_origin_x =
        (int64_t)pointer_x - WINDOW_CURSOR_HOTSPOT_X;
    new_origin_y =
        (int64_t)pointer_y - WINDOW_CURSOR_HOTSPOT_Y;

    for (uint32_t row = 0U; row < WINDOW_CURSOR_HEIGHT; ++row) {
        cursor_publish_row(new_origin_x,
                           new_origin_y + (int64_t)row,
                           row, true);
    }

    __asm__ volatile ("sfence" : : : "memory");

    g_window_server.presented_pointer_x = pointer_x;
    g_window_server.presented_pointer_y = pointer_y;
    g_window_server.presented_pointer_valid = true;

    if (out_left != 0) *out_left = new_origin_x;
    if (out_top != 0) *out_top = new_origin_y;
    if (out_right != 0) {
        *out_right = new_origin_x + (int64_t)WINDOW_CURSOR_WIDTH;
    }
    if (out_bottom != 0) {
        *out_bottom = new_origin_y + (int64_t)WINDOW_CURSOR_HEIGHT;
    }

    sched_preempt_enable();
    return true;
}
