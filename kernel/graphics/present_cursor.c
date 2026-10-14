#include <kernel/display.h>
#include <kernel/qemu_stdvga.h>
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
                               uint32_t cursor_row, bool draw_cursor, uint32_t shape) {
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
            uint32_t cursor = window_cursor_pixel(shape, source_column, cursor_row);
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

/*
 * Compose the cursor into a hidden QEMU StdVGA page before that page becomes
 * visible.  The retained composite framebuffer is cursor-free, so it is the
 * authoritative background for the small cursor rectangle; this also avoids
 * reading WC VRAM while preparing the next page.
 */
bool window_present_cursor_into_buffer(volatile uint32_t *destination,
                                       uint32_t pointer_x,
                                       uint32_t pointer_y, uint32_t shape) {
    int64_t origin_x;
    int64_t origin_y;

    if (destination == 0 ||
        g_window_server.composite_framebuffer == 0 ||
        g_window_server.display_width == 0U ||
        g_window_server.display_height == 0U) {
        return false;
    }

    origin_x = (int64_t)pointer_x - window_cursor_hotspot_x(shape);
    origin_y = (int64_t)pointer_y - window_cursor_hotspot_y(shape);

    for (uint32_t row = 0U; row < WINDOW_CURSOR_HEIGHT; ++row) {
        int64_t y = origin_y + (int64_t)row;
        int64_t left = origin_x < 0 ? 0 : origin_x;
        int64_t right = origin_x + (int64_t)WINDOW_CURSOR_WIDTH;
        uint32_t pixels;
        uint32_t scanline[WINDOW_CURSOR_WIDTH];

        if (y < 0 || y >= (int64_t)g_window_server.display_height) {
            continue;
        }
        if (right > (int64_t)g_window_server.display_width) {
            right = g_window_server.display_width;
        }
        if (left >= right) {
            continue;
        }

        pixels = (uint32_t)(right - left);
        for (uint32_t column = 0U; column < pixels; ++column) {
            uint32_t x = (uint32_t)left + column;
            uint64_t offset = (uint64_t)(uint32_t)y *
                              g_window_server.display_stride + x;
            uint32_t background =
                g_window_server.composite_framebuffer[offset];
            uint32_t source_column =
                (uint32_t)((int64_t)x - origin_x);
            uint32_t cursor = window_cursor_pixel(shape, source_column, row);

            scanline[column] = cursor_blend_xrgb(background, cursor);
        }

        display_core_publish_xrgb8888_span(
            destination +
                (uint64_t)(uint32_t)y * g_window_server.display_stride +
                (uint32_t)left,
            scanline,
            pixels);
    }

    return true;
}

/*
 * Publish one scanline while preserving the cursor already visible on the
 * fixed QEMU page.  A client repaint must not first write the cursor-free
 * background and then repair the cursor in a second transaction: QEMU can
 * expose that interval while the scanout page is being updated.
 *
 * The caller uses the return value to fall back to the ordinary copy when the
 * span does not intersect the current cursor (or when this is not the QEMU
 * single-page path).
 */
bool window_present_cursor_publish_span(
    volatile uint32_t *destination,
    const uint32_t *source,
    uint32_t x,
    uint32_t y,
    uint32_t pixels) {
    int64_t origin_x;
    int64_t origin_y;
    int64_t span_left;
    int64_t span_right;
    int64_t cursor_left;
    int64_t cursor_right;
    int64_t overlap_left;
    int64_t overlap_right;
    uint32_t cursor_row;

    if (destination == 0 || source == 0 || pixels == 0U ||
        qemu_stdvga_page_flip_active() ||
        !g_window_server.presented_pointer_valid ||
        y >= g_window_server.display_height ||
        x > g_window_server.display_width ||
        pixels > g_window_server.display_width - x) {
        return false;
    }

    origin_x =
        (int64_t)g_window_server.presented_pointer_x -
        window_cursor_hotspot_x(g_window_server.presented_cursor_shape);
    origin_y =
        (int64_t)g_window_server.presented_pointer_y -
        window_cursor_hotspot_y(g_window_server.presented_cursor_shape);
    span_left = x;
    span_right = (int64_t)x + pixels;
    cursor_left = origin_x;
    cursor_right = origin_x + WINDOW_CURSOR_WIDTH;

    overlap_left = span_left > cursor_left ? span_left : cursor_left;
    overlap_right = span_right < cursor_right ? span_right : cursor_right;
    if (overlap_left >= overlap_right ||
        y < (uint32_t)(origin_y < 0 ? 0 : origin_y) ||
        (int64_t)y >= origin_y + WINDOW_CURSOR_HEIGHT) {
        return false;
    }

    cursor_row = (uint32_t)((int64_t)y - origin_y);

    if (overlap_left > span_left) {
        uint32_t count = (uint32_t)(overlap_left - span_left);
        display_core_publish_xrgb8888_span(
            destination,
            source,
            count);
    }

    {
        uint32_t count = (uint32_t)(overlap_right - overlap_left);
        uint32_t blended[WINDOW_CURSOR_WIDTH];
        uint32_t source_offset =
            (uint32_t)(overlap_left - span_left);

        for (uint32_t column = 0U; column < count; ++column) {
            uint32_t screen_x =
                (uint32_t)overlap_left + column;
            uint32_t cursor_column =
                (uint32_t)((int64_t)screen_x - origin_x);
            blended[column] = cursor_blend_xrgb(
                source[source_offset + column],
                window_cursor_pixel(g_window_server.presented_cursor_shape,
                                    cursor_column, cursor_row));
        }

        display_core_publish_xrgb8888_span(
            destination + (overlap_left - span_left),
            blended,
            count);
    }

    if (overlap_right < span_right) {
        uint32_t source_offset =
            (uint32_t)(overlap_right - span_left);
        uint32_t count = (uint32_t)(span_right - overlap_right);

        display_core_publish_xrgb8888_span(
            destination + source_offset,
            source + source_offset,
            count);
    }

    return true;
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
    uint32_t shape;
    int64_t old_origin_x;
    int64_t old_origin_y;
    int64_t new_origin_x;
    int64_t new_origin_y;

    /* Native GB205 scanout receives the cursor in the submitted frame copy;
     * never write the inactive firmware GOP mapping as a direct overlay. */
    if (display_core_has_native_scanout(0U)) {
        return false;
    }

    if (g_window_server.framebuffer == 0 ||
        g_window_server.composite_framebuffer == 0 ||
        g_window_server.composite_framebuffer ==
            g_window_server.framebuffer ||
        g_window_server.display_width == 0U ||
        g_window_server.display_height == 0U) {
        return false;
    }

    window_lock();
    pointer_x = g_window_server.pointer_x;
    pointer_y = g_window_server.pointer_y;
    shape = window_input_cursor_shape_locked();
    window_unlock();

    if (!force &&
        g_window_server.presented_pointer_valid &&
        g_window_server.presented_pointer_x == pointer_x &&
        g_window_server.presented_pointer_y == pointer_y &&
        g_window_server.presented_cursor_shape == shape) {
        return false;
    }

    sched_preempt_disable();

    if (g_window_server.presented_pointer_valid) {
        old_origin_x =
            (int64_t)g_window_server.presented_pointer_x -
            window_cursor_hotspot_x(g_window_server.presented_cursor_shape);
        old_origin_y =
            (int64_t)g_window_server.presented_pointer_y -
            window_cursor_hotspot_y(g_window_server.presented_cursor_shape);

        for (uint32_t row = 0U; row < WINDOW_CURSOR_HEIGHT; ++row) {
            cursor_publish_row(old_origin_x,
                               old_origin_y + (int64_t)row,
                               row, false, g_window_server.presented_cursor_shape);
        }
    }

    new_origin_x =
        (int64_t)pointer_x - window_cursor_hotspot_x(shape);
    new_origin_y =
        (int64_t)pointer_y - window_cursor_hotspot_y(shape);

    for (uint32_t row = 0U; row < WINDOW_CURSOR_HEIGHT; ++row) {
        cursor_publish_row(new_origin_x,
                           new_origin_y + (int64_t)row,
                           row, true, shape);
    }

    __asm__ volatile ("sfence" : : : "memory");

    g_window_server.presented_pointer_x = pointer_x;
    g_window_server.presented_pointer_y = pointer_y;
    g_window_server.presented_cursor_shape = shape;
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
