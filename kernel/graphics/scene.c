#include "internal.h"

window_server_window_t *window_scene_find_locked(uint32_t identifier) {
    if (identifier == 0U) return 0;
    for (uint32_t index = 0U; index < g_window_server.count; ++index) {
        window_server_window_t *window = g_window_server.windows[index];
        if (window != 0 && window->identifier == identifier) return window;
    }
    return 0;
}

window_server_window_t *window_scene_keyboard_locked(void) {
    window_server_window_t *window =
        window_scene_find_locked(g_window_server.focused_identifier);
    if (window != 0 && !window->minimized &&
        (window->flags & OS_WINDOW_VISIBLE) != 0U) return window;
    for (uint32_t index = g_window_server.count; index != 0U; --index) {
        window = g_window_server.windows[index - 1U];
        if (window != 0 && !window->minimized &&
            (window->flags & OS_WINDOW_VISIBLE) != 0U) {
            window_scene_set_focus_identifier_locked(window->identifier);
            return window;
        }
    }
    return 0;
}
