/* REFACTOR_P7B_SURFACE_RENDER_OWNER */

#include "internal.h"

void compositor_draw_window_locked(
    const compositor_window_view_t *window) {

    uint32_t frame_color;

    if (window == 0) {
        return;
    }

    frame_color =
        window->identifier ==
        g_compositor_snapshot.focused_identifier ?
            0x005F8FC4U :
            0x0028323EU;

    if (!window_client_decorations(window->flags) &&
        !compositor_damage_inside_surface_interior(window)) {
        compositor_titlebar_locked(
            window,
            frame_color);
    }

    compositor_surface_locked(
        window);
}

void compositor_surface_locked(
    const compositor_window_view_t *window) {

    compositor_surface_source_context_t source_context;
    uint64_t cached_page_index = UINT64_MAX;
    uint8_t *cached_base = 0;
    uint64_t source_row_offset;
    bool private_wb;
    uint32_t *private_scene;

    int64_t frame_width;
    int64_t frame_height;
    int64_t surface_x;
    int64_t surface_y;
    int64_t first_row;
    int64_t last_row;
    int64_t base_first_column;
    int64_t base_last_column;

    if (window == 0) {
        return;
    }

    frame_width =
        (int64_t)window->width +
        window_frame_extra(window->flags);

    frame_height =
        (int64_t)window->height +
        window_frame_extra(window->flags) +
        window_titlebar_height(window->flags);

    surface_x =
        (int64_t)window->x +
        window_frame_border(window->flags);

    surface_y =
        (int64_t)window->y +
        window_client_offset_y(window->flags);

    /*
     * Compute the display/damage intersection once.
     *
     * The old path iterated row=0..window->height for every damage rectangle
     * and rejected unrelated rows inside the loop.  A 20-pixel-high update on
     * a 1000-pixel-tall window therefore still executed ~1000 row tests.
     */
    first_row = 0;
    last_row = window->height;

    if (surface_y + first_row < 0) {
        first_row = -surface_y;
    }

    if (surface_y + last_row >
        (int64_t)g_window_server.display_height) {
        last_row =
            (int64_t)g_window_server.display_height -
            surface_y;
    }

    if (surface_y + first_row <
        (int64_t)g_compositor_snapshot.damage_bounds.y0) {
        first_row =
            (int64_t)g_compositor_snapshot.damage_bounds.y0 -
            surface_y;
    }

    if (surface_y + last_row >
        (int64_t)g_compositor_snapshot.damage_bounds.y1) {
        last_row =
            (int64_t)g_compositor_snapshot.damage_bounds.y1 -
            surface_y;
    }

    if (first_row < 0) {
        first_row = 0;
    }

    if (last_row > (int64_t)window->height) {
        last_row = window->height;
    }

    if (first_row >= last_row) {
        return;
    }

    base_first_column = 0;
    base_last_column = window->width;

    if (surface_x + base_first_column < 0) {
        base_first_column = -surface_x;
    }

    if (surface_x + base_last_column >
        (int64_t)g_window_server.display_width) {
        base_last_column =
            (int64_t)g_window_server.display_width -
            surface_x;
    }

    if (surface_x + base_first_column <
        (int64_t)g_compositor_snapshot.damage_bounds.x0) {
        base_first_column =
            (int64_t)g_compositor_snapshot.damage_bounds.x0 -
            surface_x;
    }

    if (surface_x + base_last_column >
        (int64_t)g_compositor_snapshot.damage_bounds.x1) {
        base_last_column =
            (int64_t)g_compositor_snapshot.damage_bounds.x1 -
            surface_x;
    }

    if (base_first_column < 0) {
        base_first_column = 0;
    }

    if (base_last_column > (int64_t)window->width) {
        base_last_column = window->width;
    }

    if (base_first_column >= base_last_column) {
        return;
    }

    compositor_surface_source_prepare(window, &source_context);

    source_row_offset =
        (uint64_t)(uint32_t)first_row *
        source_context.row_bytes;

    private_wb =
        g_window_server.composite_framebuffer != 0 &&
        g_window_server.composite_framebuffer !=
            g_window_server.framebuffer;

    private_scene =
        private_wb ?
            (uint32_t *)(uintptr_t)
                g_window_server.composite_framebuffer :
            0;

    for (uint32_t row = (uint32_t)first_row;
         row < (uint32_t)last_row;
         ++row) {

        uint64_t row_source_offset =
            source_row_offset;

        source_row_offset +=
            source_context.row_bytes;

        int64_t destination_y =
            surface_y + row;

        int64_t frame_row =
            destination_y -
            window->y;

        int64_t first_column =
            base_first_column;

        int64_t last_column =
            base_last_column;

        uint32_t inset;
        uint64_t destination_index;
        uint32_t *wb_destination;
        volatile uint32_t *device_destination;

        inset = (frame_row >= 0 && frame_row < frame_height ?
                 compositor_corner_inset(
                     (uint32_t)frame_row,
                     (uint32_t)frame_width,
                     (uint32_t)frame_height) :
                 0U);

        /*
         * Preserve the one-pixel decoration border around rounded windows.
         * This is the only row-dependent clipping left in the hot loop.
         */
        if (first_column <
            (int64_t)inset) {
            first_column = inset;
        }

        if (last_column >
            (int64_t)window->width -
                inset) {
            last_column =
                (int64_t)window->width -
                inset;
        }

        if (first_column >= last_column) {
            continue;
        }

        destination_index =
            (uint64_t)
                (uint32_t)destination_y *
                g_window_server.display_stride +

            (uint32_t)(
                surface_x +
                first_column);

        if (private_wb) {
            wb_destination =
                private_scene + destination_index;
            device_destination = 0;
        } else {
            wb_destination = 0;
            device_destination =
                g_window_server.composite_framebuffer +
                destination_index;
        }

        compositor_copy_surface_span(
            window,
            &source_context,
            row_source_offset,
            (uint32_t)first_column,
            (uint32_t)last_column,
            wb_destination,
            device_destination,
            &cached_page_index,
            &cached_base);
    }
}

void compositor_surface_source_prepare(
    const compositor_window_view_t *window,
    compositor_surface_source_context_t *context) {

    vm_object_t *object;

    if (context == 0) return;
    *context = (compositor_surface_source_context_t){0};
    if (window == 0) return;

    context->row_bytes =
        (uint64_t)window->width * sizeof(uint32_t);

    if (window->resize_pending ||
        window->section == 0 ||
        window->section->vm_object == 0 ||
        window->owner_address == 0U) {
        return;
    }

    object = window->section->vm_object;
    if (object->type != VM_OBJECT_SHARED) return;

    context->object = object;
    context->cache = compositor_surface_cache_get(window);
    context->readable = true;
}

void compositor_copy_surface_span(
    const compositor_window_view_t *window,
    const compositor_surface_source_context_t *surface,
    uint64_t source_row_offset,
    uint32_t first_column,
    uint32_t last_column,
    uint32_t *wb_destination,
    volatile uint32_t *device_destination,
    uint64_t *cached_page_index,
    uint8_t **cached_base) {

    uint64_t source_offset;
    uint64_t column_offset;
    uint32_t remaining;

    if (window == 0 || surface == 0 ||
        (wb_destination == 0 && device_destination == 0) ||
        cached_page_index == 0 || cached_base == 0 ||
        first_column >= last_column) {
        return;
    }

    remaining = last_column - first_column;

    if (!surface->readable) {
        compositor_surface_fill_destination(
            wb_destination, device_destination,
            remaining, window->background);
        return;
    }

    column_offset =
        (uint64_t)first_column * sizeof(uint32_t);

    if (source_row_offset > UINT64_MAX - column_offset) {
        compositor_surface_fill_destination(
            wb_destination, device_destination,
            remaining, window->background);
        return;
    }

    source_offset = source_row_offset + column_offset;

    if (source_offset >= window->buffer_size) {
        compositor_surface_fill_destination(
            wb_destination, device_destination,
            remaining, window->background);
        return;
    }

    {
        uint64_t available_bytes =
            window->buffer_size - source_offset;
        uint64_t requested_bytes =
            (uint64_t)remaining * sizeof(uint32_t);

        if (requested_bytes > available_bytes) {
            uint32_t available_pixels =
                (uint32_t)(available_bytes / sizeof(uint32_t));

            if (available_pixels < remaining) {
                if (wb_destination != 0) {
                    compositor_fill_wb_pixels(
                        wb_destination + available_pixels,
                        remaining - available_pixels,
                        window->background);
                } else {
                    compositor_fill_surface_pixels(
                        device_destination + available_pixels,
                        remaining - available_pixels,
                        window->background);
                }
                remaining = available_pixels;
            }
        }
    }

    while (remaining != 0U) {
        uint64_t page_index = source_offset >> PAGE_SHIFT;
        uint32_t in_page =
            (uint32_t)(source_offset & (PAGE_SIZE - 1U));
        uint32_t page_pixels =
            (PAGE_SIZE - in_page) / sizeof(uint32_t);
        uint32_t chunk =
            remaining < page_pixels ? remaining : page_pixels;

        if (*cached_page_index != page_index) {
            *cached_base =
                compositor_surface_page_resolve(surface, page_index);
            *cached_page_index = page_index;
        }

        if (*cached_base != 0) {
            const uint32_t *source =
                (const uint32_t *)(const void *)(*cached_base + in_page);
            compositor_surface_copy_destination(
                wb_destination, device_destination,
                source, chunk);
        } else {
            compositor_surface_fill_destination(
                wb_destination, device_destination,
                chunk, window->background);
        }

        if (wb_destination != 0) {
            wb_destination += chunk;
        } else {
            device_destination += chunk;
        }

        source_offset += (uint64_t)chunk * sizeof(uint32_t);
        remaining -= chunk;
    }
}

