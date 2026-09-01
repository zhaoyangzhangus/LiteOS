#include "internal.h"

/* REFACTOR_P7A_INPUT_DRAG_OWNER: retained drag and resize geometry policy. */

/* Drag capture is input state. All mutations stay here so a cancellation
 * cannot leave an identifier, resize edge, or retained-copy flag behind. */
void window_input_reset_drag_locked(void) {
    g_window_server.dragging_identifier = 0U;
    g_window_server.drag_offset_x = 0;
    g_window_server.drag_offset_y = 0;
    g_window_server.resize_edges = 0U;
    g_window_server.drag_blit_valid = false;
    g_window_server.drag_old_x = 0;
    g_window_server.drag_old_y = 0;
    g_window_server.drag_new_x = 0;
    g_window_server.drag_new_y = 0;
    g_window_server.drag_width = 0U;
    g_window_server.drag_height = 0U;
}

void window_input_clear_drag_locked(void) {
    g_window_server.dragging_identifier = 0U;
    g_window_server.drag_offset_x = 0;
    g_window_server.drag_offset_y = 0;
    g_window_server.resize_edges = 0U;
    g_window_server.drag_blit_valid = false;
}

void window_input_begin_drag_locked(uint32_t identifier,
                                    int32_t offset_x,
                                    int32_t offset_y,
                                    uint32_t resize_edges) {
    g_window_server.dragging_identifier = identifier;
    g_window_server.drag_offset_x = offset_x;
    g_window_server.drag_offset_y = offset_y;
    g_window_server.resize_edges = resize_edges;
}

void window_input_set_drag_blit_valid_locked(bool valid) {
    g_window_server.drag_blit_valid = valid;
}

void window_input_record_drag_frame_locked(
    bool blit_valid,
    int32_t old_x, int32_t old_y,
    int32_t new_x, int32_t new_y,
    uint32_t width, uint32_t height) {
    g_window_server.drag_blit_valid = blit_valid;
    g_window_server.drag_old_x = old_x;
    g_window_server.drag_old_y = old_y;
    g_window_server.drag_new_x = new_x;
    g_window_server.drag_new_y = new_y;
    g_window_server.drag_width = width;
    g_window_server.drag_height = height;
}

static __attribute__((unused)) bool window_drag_reuse_available_locked(
    const window_server_window_t *window) {
    return window != 0 &&
           g_window_server.resize_edges == 0U &&
           g_window_server.composite_framebuffer != 0 &&
           g_window_server.composite_framebuffer !=
               g_window_server.framebuffer &&
           g_window_server.count != 0U &&
           g_window_server.windows[g_window_server.count - 1U] == window;
}

/*
 * Retained dragging is valid only when old_x/old_y still describes the
 * pixels currently stored in composite_framebuffer. More than one HID report
 * can be consumed before a frame is composed, so an intermediate logical
 * position must not be reused as if it had already been presented.
 */
bool window_drag_reuse_safe_locked(
    const window_server_window_t *window,
    int32_t old_x, int32_t old_y,
    int32_t new_x, int32_t new_y,
    uint32_t width, uint32_t height) {
#if WINDOW_COMPOSITOR_RETAINED_DRAG == 0U
    (void)window;
    (void)old_x;
    (void)old_y;
    (void)new_x;
    (void)new_y;
    (void)width;
    (void)height;
    return false;
#else
    if (!window_drag_reuse_available_locked(window) ||
        g_window_server.dirty ||
        g_window_server.composing ||
        width == 0U || height == 0U ||
        old_x < 0 || old_y < 0 ||
        new_x < 0 || new_y < 0 ||
        !window->compositor_presented_valid ||
        window->compositor_presented_x != old_x ||
        window->compositor_presented_y != old_y ||
        window->compositor_presented_width != width ||
        window->compositor_presented_height != height) {
        return false;
    }

    return (uint64_t)(uint32_t)old_x + width <=
               g_window_server.display_width &&
           (uint64_t)(uint32_t)new_x + width <=
               g_window_server.display_width &&
           (uint64_t)(uint32_t)old_y + height <=
               g_window_server.display_height &&
           (uint64_t)(uint32_t)new_y + height <=
               g_window_server.display_height;
#endif
}

uint32_t window_resize_edges_locked(const window_server_window_t *window,
                                    uint32_t x, uint32_t y) {
    return window_input_resize_edges_locked(window, x, y);
}

bool window_resize_locked(window_server_window_t *window,
                          int32_t delta_x, int32_t delta_y) {
    int32_t old_x;
    int32_t old_y;
    uint32_t old_width;
    uint32_t old_height;
    uint32_t max_width;
    uint32_t max_height;
    uint32_t min_width;
    uint32_t min_height;
    int64_t left;
    int64_t top;
    int64_t right;
    int64_t bottom;
    int64_t minimum;
    int64_t maximum;
    uint32_t new_width;
    uint32_t new_height;

    if (window == 0 || g_window_server.resize_edges == 0U ||
        (window->flags & OS_WINDOW_RESIZABLE) == 0U) {
        return false;
    }

    max_width = g_window_server.display_width;
    max_height = g_window_server.display_height;
    if (max_width == 0U || max_height == 0U) return false;
    min_width = max_width < WINDOW_MIN_WIDTH ? max_width : WINDOW_MIN_WIDTH;
    min_height = max_height < WINDOW_MIN_HEIGHT ? max_height : WINDOW_MIN_HEIGHT;

    old_x = window->x;
    old_y = window->y;
    old_width = window->width;
    old_height = window->height;

    left = window->x;
    top = window->y;
    right = left +
            (int64_t)window_outer_width(window->width, window->flags);
    bottom =
        top +
        (int64_t)window_outer_height(window->height, window->flags);

    if ((g_window_server.resize_edges & WINDOW_RESIZE_LEFT) != 0U) {
        left += delta_x;
        minimum = right - (int64_t)max_width -
                  (int64_t)window_frame_extra(window->flags);
        maximum = right - (int64_t)min_width -
                  (int64_t)window_frame_extra(window->flags);
        if (left < minimum) left = minimum;
        if (left > maximum) left = maximum;
    } else if ((g_window_server.resize_edges & WINDOW_RESIZE_RIGHT) != 0U) {
        right += delta_x;
        minimum = left + (int64_t)min_width +
                  (int64_t)window_frame_extra(window->flags);
        maximum = left + (int64_t)max_width +
                  (int64_t)window_frame_extra(window->flags);
        if (right < minimum) right = minimum;
        if (right > maximum) right = maximum;
    }

    if ((g_window_server.resize_edges & WINDOW_RESIZE_TOP) != 0U) {
        top += delta_y;
        minimum =
            bottom -
            (int64_t)max_height -
            (int64_t)window_frame_extra(window->flags) -
            (int64_t)window_titlebar_height(window->flags);
        maximum =
            bottom -
            (int64_t)min_height -
            (int64_t)window_frame_extra(window->flags) -
            (int64_t)window_titlebar_height(window->flags);
        if (top < minimum) top = minimum;
        if (top > maximum) top = maximum;
    } else if ((g_window_server.resize_edges & WINDOW_RESIZE_BOTTOM) != 0U) {
        bottom += delta_y;
        minimum =
            top +
            (int64_t)min_height +
            (int64_t)window_frame_extra(window->flags) +
            (int64_t)window_titlebar_height(window->flags);
        maximum =
            top +
            (int64_t)max_height +
            (int64_t)window_frame_extra(window->flags) +
            (int64_t)window_titlebar_height(window->flags);
        if (bottom < minimum) bottom = minimum;
        if (bottom > maximum) bottom = maximum;
    }

    new_width = (uint32_t)(right - left -
                           (int64_t)window_frame_extra(window->flags));
    new_height =
        (uint32_t)(
            bottom -
            top -
            (int64_t)window_frame_extra(window->flags) -
            (int64_t)window_titlebar_height(window->flags));
    if (left == old_x && top == old_y &&
        new_width == old_width && new_height == old_height) {
        return false;
    }

    window_mark_rect_locked(old_x, old_y,
                            window_outer_width(old_width, window->flags),
                            window_outer_height(old_height, window->flags));
    window->x = (int32_t)left;
    window->y = (int32_t)top;
    window->width = new_width;
    window->height = new_height;
    window->dirty = true;
    window->resize_pending = true;
    window_mark_window_locked(window);
    window_enqueue_resize_event_locked(window);
    return true;
}

/*
 * Apply one accumulated X/Y motion burst to an active Ring0 decoration
 * capture. Only drag/resize is consumed by Ring0; ordinary pointer motion is
 * still routed through the normal per-axis path in input_motion.c.
 */
bool window_route_drag_motion_batch_locked(
    const window_motion_batch_t *batch) {
    window_server_window_t *dragged;
    uint32_t old_pointer_x;
    uint32_t old_pointer_y;
    int64_t next_x;
    int64_t next_y;
    int32_t actual_delta_x;
    int32_t actual_delta_y;

    if (batch == 0 || g_window_server.dragging_identifier == 0U) {
        return false;
    }

    dragged = window_scene_find_locked(g_window_server.dragging_identifier);
    if (dragged == 0 ||
        (dragged->flags & OS_WINDOW_VISIBLE) == 0U) {
        window_input_clear_drag_locked();
        return false;
    }

    old_pointer_x = g_window_server.pointer_x;
    old_pointer_y = g_window_server.pointer_y;
    next_x = (int64_t)old_pointer_x + batch->delta_x;
    next_y = (int64_t)old_pointer_y + batch->delta_y;
    if (next_x < 0) next_x = 0;
    if (next_y < 0) next_y = 0;
    if (g_window_server.display_width == 0U) {
        next_x = 0;
    } else if (next_x >= (int64_t)g_window_server.display_width) {
        next_x = (int64_t)g_window_server.display_width - 1;
    }
    if (g_window_server.display_height == 0U) {
        next_y = 0;
    } else if (next_y >= (int64_t)g_window_server.display_height) {
        next_y = (int64_t)g_window_server.display_height - 1;
    }

    window_input_set_pointer_locked((uint32_t)next_x, (uint32_t)next_y);
    if (g_window_server.desktop_hovered_app != DESKTOP_APP_NONE) {
        desktop_mark_app_locked(g_window_server.desktop_hovered_app);
        desktop_set_hovered_app_locked(DESKTOP_APP_NONE);
    }

    actual_delta_x = (int32_t)((int64_t)g_window_server.pointer_x -
                               (int64_t)old_pointer_x);
    actual_delta_y = (int32_t)((int64_t)g_window_server.pointer_y -
                               (int64_t)old_pointer_y);
    if (g_window_server.resize_edges != 0U) {
        window_input_set_drag_blit_valid_locked(false);
        if (actual_delta_x != 0 || actual_delta_y != 0) {
            (void)window_resize_locked(dragged, actual_delta_x, actual_delta_y);
        }
    } else {
        int32_t old_x = dragged->x;
        int32_t old_y = dragged->y;
        int32_t new_x = (int32_t)g_window_server.pointer_x -
                        g_window_server.drag_offset_x;
        int32_t new_y = (int32_t)g_window_server.pointer_y -
                        g_window_server.drag_offset_y;

        if (old_x != new_x || old_y != new_y) {
            uint32_t outer_width =
                window_outer_width(dragged->width, dragged->flags);
            uint32_t outer_height =
                window_outer_height(dragged->height, dragged->flags);
            int32_t retained_old_x = dragged->compositor_presented_x;
            int32_t retained_old_y = dragged->compositor_presented_y;
            bool reuse_retained = window_drag_reuse_safe_locked(
                dragged, retained_old_x, retained_old_y,
                new_x, new_y, outer_width, outer_height);
            int32_t damage_old_x = reuse_retained ? retained_old_x : old_x;
            int32_t damage_old_y = reuse_retained ? retained_old_y : old_y;

            window_input_record_drag_frame_locked(
                reuse_retained,
                damage_old_x, damage_old_y,
                new_x, new_y,
                outer_width, outer_height);
            dragged->x = new_x;
            dragged->y = new_y;
            window_mark_moved_rect_locked(
                damage_old_x, damage_old_y, new_x, new_y,
                outer_width, outer_height, !reuse_retained);
            if (reuse_retained) {
                window_mark_drag_corner_repair_locked(
                    new_x, new_y, outer_width, outer_height, dragged->flags);
            }
            dragged->dirty = true;
        }
    }

    if (old_pointer_x != g_window_server.pointer_x ||
        old_pointer_y != g_window_server.pointer_y) {
        window_mark_moved_cursor_locked(
            old_pointer_x, old_pointer_y,
            g_window_server.pointer_x, g_window_server.pointer_y);
    }
    window_coalesce_damage_locked();
    return true;
}
