#include <kernel/kmem.h>
#include "internal.h"

/* The compositor writes the retained scene here before scanout publication. */
void window_buffer_reset_locked(void) {
    g_window_server.composite_framebuffer = 0;
}

void window_buffer_set_target_locked(volatile uint32_t *target) {
    g_window_server.composite_framebuffer = target;
}

void window_buffer_prepare(void) {
    if (g_window_server.composite_framebuffer != 0) return;

    uint64_t bytes =
        (uint64_t)g_window_server.display_stride *
        g_window_server.display_height * sizeof(uint32_t);

    window_buffer_set_target_locked(
        (volatile uint32_t *)kzalloc((size_t)bytes, 0));
    if (g_window_server.composite_framebuffer == 0) {
        /* Preserve the existing low-memory direct-framebuffer fallback. */
        window_buffer_set_target_locked(g_window_server.framebuffer);
    }
}
