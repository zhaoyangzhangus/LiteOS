#include "internal.h"

/* Scene/Z-order is the sole mutation Owner for the focused window id. */
void window_scene_set_focus_identifier_locked(uint32_t identifier) {
    g_window_server.focused_identifier = identifier;
}

void window_scene_focus_locked(window_server_window_t *window) {
    window_server_window_t *old_focused;
    uint32_t position = 0U;

    if (window == 0 || window->minimized ||
        (window->flags & OS_WINDOW_VISIBLE) == 0U) return;
    old_focused = window_scene_find_locked(g_window_server.focused_identifier);
    for (; position < g_window_server.count; ++position) {
        if (g_window_server.windows[position] == window) break;
    }
    /* Re-focusing the frontmost window must not create new damage. */
    if (old_focused == window && position + 1U >= g_window_server.count) {
        return;
    }
    window_registry_move_to_front_locked(window);
    window_scene_set_focus_identifier_locked(window->identifier);
    window_mark_window_locked(old_focused);
    window_mark_window_locked(window);
}
