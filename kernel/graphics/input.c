#include "internal.h"

uint32_t window_input_resize_edges_locked(const window_server_window_t *window,
                                          uint32_t x, uint32_t y) {
    int64_t relative_x;
    int64_t relative_y;
    int64_t outer_width;
    int64_t outer_height;
    uint32_t edges = 0U;

    if (window == 0 || window->maximized ||
        (window->flags & OS_WINDOW_RESIZABLE) == 0U) return 0U;
    relative_x = (int64_t)x - window->x;
    relative_y = (int64_t)y - window->y;
    outer_width = (int64_t)window_outer_width(window->width, window->flags);
    outer_height = (int64_t)window_outer_height(window->height, window->flags);
    if (relative_x < 0 || relative_y < 0 ||
        relative_x >= outer_width || relative_y >= outer_height) return 0U;
    if (relative_x < WINDOW_RESIZE_GRAB) edges |= WINDOW_RESIZE_LEFT;
    else if (relative_x >= outer_width - WINDOW_RESIZE_GRAB) {
        edges |= WINDOW_RESIZE_RIGHT;
    }
    if (relative_y < WINDOW_RESIZE_GRAB) edges |= WINDOW_RESIZE_TOP;
    else if (relative_y >= outer_height - WINDOW_RESIZE_GRAB) {
        edges |= WINDOW_RESIZE_BOTTOM;
    }
    return edges;
}
