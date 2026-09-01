#include <ascii_font.h>
#include "internal.h"


/* REFACTOR_P7C_DECORATION_OWNER: titlebar, controls, and window chrome. */

static void compositor_put_pixel_locked(int32_t x, int32_t y,
                                            uint32_t color) {
    if (g_window_server.composite_framebuffer == 0 ||
        x < 0 ||
        y < 0 ||
        x >= (int32_t)g_window_server.display_width ||
        y >= (int32_t)g_window_server.display_height ||
        x < (int32_t)g_compositor_snapshot.damage_bounds.x0 ||
        y < (int32_t)g_compositor_snapshot.damage_bounds.y0 ||
        x >= (int32_t)g_compositor_snapshot.damage_bounds.x1 ||
        y >= (int32_t)g_compositor_snapshot.damage_bounds.y1) {
        return;
    }

    g_window_server.composite_framebuffer[
        (uint64_t)(uint32_t)y * g_window_server.display_stride +
        (uint32_t)x] = color;
}

static void compositor_blend_pixel_locked(int32_t x, int32_t y,
                                           uint32_t color, uint32_t alpha) {
    uint32_t destination;
    uint32_t inverse;
    uint32_t red;
    uint32_t green;
    uint32_t blue;

    if (g_window_server.composite_framebuffer == 0 || alpha == 0U ||
        x < 0 || y < 0 ||
        x >= (int32_t)g_window_server.display_width ||
        y >= (int32_t)g_window_server.display_height ||
        x < (int32_t)g_compositor_snapshot.damage_bounds.x0 ||
        y < (int32_t)g_compositor_snapshot.damage_bounds.y0 ||
        x >= (int32_t)g_compositor_snapshot.damage_bounds.x1 ||
        y >= (int32_t)g_compositor_snapshot.damage_bounds.y1) {
        return;
    }

    if (alpha >= 255U) {
        compositor_put_pixel_locked(x, y, color);
        return;
    }

    destination = g_window_server.composite_framebuffer[
        (uint64_t)(uint32_t)y * g_window_server.display_stride +
        (uint32_t)x];
    inverse = 255U - alpha;
    red = (((color >> 16U) & 0xFFU) * alpha +
           ((destination >> 16U) & 0xFFU) * inverse + 127U) / 255U;
    green = (((color >> 8U) & 0xFFU) * alpha +
             ((destination >> 8U) & 0xFFU) * inverse + 127U) / 255U;
    blue = ((color & 0xFFU) * alpha +
            (destination & 0xFFU) * inverse + 127U) / 255U;
    g_window_server.composite_framebuffer[
        (uint64_t)(uint32_t)y * g_window_server.display_stride +
        (uint32_t)x] = (destination & 0xFF000000U) |
                        (red << 16U) | (green << 8U) | blue;
}

void compositor_draw_small_glyph_locked(int32_t x, int32_t y,
                                        char character, uint32_t color) {
    const UINT8 *glyph = ascii_font_glyph((UINT8)character);
    uint32_t width = ascii_font_width();
    uint32_t height = ascii_font_height();

    if (glyph == 0) return;
    if (width == 0U || height == 0U) return;

    for (uint32_t row = 0U; row < height; ++row) {
        for (uint32_t column = 0U; column < width; ++column) {
            compositor_blend_pixel_locked(
                x + (int32_t)column,
                y + (int32_t)row,
                color,
                glyph[row * width + column]);
        }
    }
}


static void compositor_title_controls_locked(
    const compositor_window_view_t *window) {

    static const uint32_t buttons[] = {
        WINDOW_TITLE_BUTTON_MINIMIZE,
        WINDOW_TITLE_BUTTON_MAXIMIZE,
        WINDOW_TITLE_BUTTON_CLOSE,
    };

    bool focused;

    if (window == 0) {
        return;
    }

    if (window_client_decorations(window->flags)) {
        return;
    }
    focused =
        window->identifier ==
        g_compositor_snapshot.focused_identifier;

    for (uint32_t index = 0U;
         index <
             sizeof(buttons) /
             sizeof(buttons[0]);
         ++index) {

        uint32_t button =
            buttons[index];

        int32_t x;
        int32_t y;

        uint32_t background;
        uint32_t glyph;

        if (!window_title_button_rect(
                window->x,
                window->y,
                window->width,
                button,
                &x,
                &y)) {
            continue;
        }

        background =
            focused ?
                0x001E2A36U :
                0x00182028U;

        glyph =
            focused ?
                0x00B7C3D0U :
                0x007F8B96U;

        /* Keep close visually identifiable without a permanent red block. */
        if (button ==
            WINDOW_TITLE_BUTTON_CLOSE) {

            background =
                focused ?
                    0x002C2228U :
                    0x00241C20U;

            glyph =
                focused ?
                    0x00E28A92U :
                    0x009B6870U;
        }

        compositor_fill_rounded_locked(
            x,
            y,
            WINDOW_TITLE_BUTTON_SIZE,
            WINDOW_TITLE_BUTTON_SIZE,
            WINDOW_CORNER_RADIUS,
            background);

        if (button ==
            WINDOW_TITLE_BUTTON_MINIMIZE) {

            compositor_fill_locked(
                x + 6,
                y + 11,
                8U,
                1U,
                glyph);

        } else if (
            button ==
            WINDOW_TITLE_BUTTON_MAXIMIZE) {

            if (!window->maximized) {
                /* Normal maximize glyph. */
                compositor_fill_locked(
                    x + 6,
                    y + 6,
                    8U,
                    1U,
                    glyph);

                compositor_fill_locked(
                    x + 6,
                    y + 13,
                    8U,
                    1U,
                    glyph);

                compositor_fill_locked(
                    x + 6,
                    y + 6,
                    1U,
                    8U,
                    glyph);

                compositor_fill_locked(
                    x + 13,
                    y + 6,
                    1U,
                    8U,
                    glyph);

            } else {
                /* Restore glyph: two overlapping rectangles. */
                compositor_fill_locked(
                    x + 8,
                    y + 6,
                    7U,
                    1U,
                    glyph);

                compositor_fill_locked(
                    x + 14,
                    y + 6,
                    1U,
                    7U,
                    glyph);

                compositor_fill_locked(
                    x + 7,
                    y + 8,
                    7U,
                    1U,
                    glyph);

                compositor_fill_locked(
                    x + 7,
                    y + 8,
                    1U,
                    7U,
                    glyph);

                compositor_fill_locked(
                    x + 7,
                    y + 14,
                    7U,
                    1U,
                    glyph);

                compositor_fill_locked(
                    x + 13,
                    y + 8,
                    1U,
                    7U,
                    glyph);
            }
        } else if (
            button ==
            WINDOW_TITLE_BUTTON_CLOSE) {

            /* Small X. */
            for (uint32_t step = 0U;
                 step < 7U;
                 ++step) {

                compositor_fill_locked(
                    x + 7 +
                        (int32_t)step,
                    y + 7 +
                        (int32_t)step,
                    1U,
                    1U,
                    glyph);

                compositor_fill_locked(
                    x + 13 -
                        (int32_t)step,
                    y + 7 +
                        (int32_t)step,
                    1U,
                    1U,
                    glyph);
            }
        }
    }
}

void compositor_titlebar_locked(
    const compositor_window_view_t *window,
    uint32_t frame_color) {

    bool focused;

    uint32_t title_color;
    uint32_t separator_color;
    uint32_t text_color;

    uint32_t outer_width;
    uint32_t outer_height;

    uint32_t max_chars;

    int32_t text_x;
    int32_t text_y;

    if (window == 0) {
        return;
    }

    if (window_client_decorations(window->flags)) {
        return;
    }
    focused =
        window->identifier ==
        g_compositor_snapshot.focused_identifier;

    title_color =
        focused ?
            0x00172230U :
            0x00131921U;

    separator_color =
        focused ?
            0x0030475FU :
            0x00232D37U;

    text_color =
        focused ?
            0x00ECF3FAU :
            0x009AA8B7U;

    outer_width =
        window->width +
        window_frame_extra(window->flags);

    outer_height =
        window->height +
        window_frame_extra(window->flags) +
        window_titlebar_height(window->flags);

    /* Complete flat outer frame. */
    compositor_fill_rounded_locked(
        window->x,
        window->y,
        outer_width,
        outer_height,
        WINDOW_CORNER_RADIUS,
        frame_color);

    /* Keep the topmost rounded corner pixels owned by the outer frame. */
    if (window->width >
        WINDOW_CORNER_RADIUS * 2U) {

        compositor_fill_locked(
            window->x +
                (int32_t)WINDOW_CORNER_RADIUS,
            window->y +
                (int32_t)window_frame_border(window->flags),
            window->width -
                WINDOW_CORNER_RADIUS * 2U,
            window_titlebar_height(window->flags),
            title_color);
    }

    /* Fill the central/lower titlebar portion. */
    if (window_titlebar_height(window->flags) >
        WINDOW_CORNER_RADIUS) {

        compositor_fill_locked(
            window->x +
                (int32_t)window_frame_border(window->flags),
            window->y +
                (int32_t)WINDOW_CORNER_RADIUS,
            window->width,
            window_titlebar_height(window->flags) -
                WINDOW_CORNER_RADIUS +
                window_frame_border(window->flags),
            title_color);
    }

    /* Client/titlebar separator. */
    compositor_fill_locked(
        window->x +
            (int32_t)window_frame_border(window->flags),
        window->y +
            (int32_t)window_client_offset_y(window->flags) -
            1,
        window->width,
        1U,
        separator_color);

    compositor_title_controls_locked(
        window);

    /* Window title uses the A8 font cache loaded from the root volume. */
    if (ascii_font_width() == 0U || ascii_font_height() == 0U) {
        return;
    }
    if (window->width <=
        WINDOW_TITLE_CONTROLS_WIDTH + ascii_font_width() * 3U) {
        return;
    }

    max_chars =
        (window->width -
         WINDOW_TITLE_CONTROLS_WIDTH -
         ascii_font_width() * 3U) /
        ascii_font_width();

    if (max_chars > 31U) {
        max_chars = 31U;
    }

    text_x =
        window->x + 12;

    text_y =
        window->y +
        (int32_t)window_frame_border(window->flags) +
        (int32_t)(window_titlebar_height(window->flags) > ascii_font_height() ?
            (window_titlebar_height(window->flags) - ascii_font_height()) /
                2U : 0U);

    for (uint32_t index = 0U;
         index < max_chars &&
         window->title[index] != '\0';
         ++index) {

        compositor_draw_small_glyph_locked(
            text_x +
                (int32_t)(index * ascii_font_width()),
            text_y,
            window->title[index],
            text_color);
    }
}
