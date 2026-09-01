#include "internal.h"

/* REFACTOR_P7B_COMPOSITOR_DRAG_OWNER: retained-scene drag overlap moves. */

/*
 * WB retained-scene qword store.
 *
 * The private composite framebuffer is ordinary cacheable RAM on this path.
 * MOVQ is used explicitly so unaligned source/destination X coordinates remain
 * valid without relying on C alignment rules.
 */
static inline void compositor_wb_store64(
    uint32_t *destination,
    uint64_t value) {

    __asm__ volatile (
        "movq %1, %0"
        : "=m"(*(uint64_t *)(void *)destination)
        : "r"(value));
}

/*
 * WB retained-scene qword load.
 *
 * P6 centralizes WB -> GOP publication in display core, but the compositor
 * still needs a WB load for the overlap-safe backward memmove used while
 * moving retained-scene pixels.  Keep this read-only helper local; it does
 * not duplicate the display-memory writer that P6 removes from this module.
 */
static inline uint64_t compositor_wb_load64(
    const uint32_t *source) {

    uint64_t value;

    __asm__ (
        "movq %1, %0"
        : "=r"(value)
        : "m"(*(const uint64_t *)(const void *)source));

    return value;
}

/*
 * Copy an overlapping WB span from high addresses toward low addresses.
 *
 * This is the only direction REP MOVSQ cannot safely provide while keeping
 * DF=0.  The kernel never sets the direction flag here: an interrupt/exception
 * may run at any instruction, and the normal x86-64 kernel ABI expects DF to
 * remain clear.
 *
 * Load a complete 64-byte block before storing any part of it.  That makes
 * even a one-pixel right shift safe when source/destination overlap inside
 * the same block.
 */
static inline void compositor_move_wb_pixels_backward(
    uint32_t *destination,
    const uint32_t *source,
    uint32_t pixels) {

    if (destination == 0 ||
        source == 0 ||
        pixels == 0U) {
        return;
    }

    if ((pixels & 1U) != 0U) {
        --pixels;
        destination[pixels] =
            source[pixels];
    }

    while (pixels >= 16U) {
        uint64_t v0;
        uint64_t v1;
        uint64_t v2;
        uint64_t v3;
        uint64_t v4;
        uint64_t v5;
        uint64_t v6;
        uint64_t v7;

        pixels -= 16U;

        v0 = compositor_wb_load64(source + pixels + 0U);
        v1 = compositor_wb_load64(source + pixels + 2U);
        v2 = compositor_wb_load64(source + pixels + 4U);
        v3 = compositor_wb_load64(source + pixels + 6U);
        v4 = compositor_wb_load64(source + pixels + 8U);
        v5 = compositor_wb_load64(source + pixels + 10U);
        v6 = compositor_wb_load64(source + pixels + 12U);
        v7 = compositor_wb_load64(source + pixels + 14U);

        __asm__ volatile ("" : : : "memory");

        compositor_wb_store64(destination + pixels + 14U, v7);
        compositor_wb_store64(destination + pixels + 12U, v6);
        compositor_wb_store64(destination + pixels + 10U, v5);
        compositor_wb_store64(destination + pixels + 8U, v4);
        compositor_wb_store64(destination + pixels + 6U, v3);
        compositor_wb_store64(destination + pixels + 4U, v2);
        compositor_wb_store64(destination + pixels + 2U, v1);
        compositor_wb_store64(destination + pixels + 0U, v0);
    }

    while (pixels >= 2U) {
        uint64_t value;

        pixels -= 2U;

        value =
            compositor_wb_load64(
                source + pixels);

        compositor_wb_store64(
            destination + pixels,
            value);
    }
}

/*
 * memmove semantics for one XRGB8888 WB scanline span.
 *
 * Forward/non-overlapping copies use the existing REP MOVSQ fast path.
 * Only an overlapping right shift needs the high->low helper above.
 */
static inline void compositor_move_wb_pixels(
    uint32_t *destination,
    const uint32_t *source,
    uint32_t pixels) {

    uintptr_t destination_address;
    uintptr_t source_address;
    uintptr_t bytes;

    if (destination == 0 ||
        source == 0 ||
        pixels == 0U ||
        destination == source) {
        return;
    }

    destination_address =
        (uintptr_t)destination;

    source_address =
        (uintptr_t)source;

    bytes =
        (uintptr_t)pixels *
        sizeof(uint32_t);

    if (destination_address < source_address ||
        destination_address >= source_address + bytes) {

        compositor_copy_wb_pixels(
            destination,
            source,
            pixels);

        return;
    }

    compositor_move_wb_pixels_backward(
        destination,
        source,
        pixels);
}

/* Move the overlap of a topmost dragged window inside the retained compose
 * buffer.  The source is the last committed frame, so this avoids rereading
 * the client section and repainting every pixel for a one-pixel motion. */
void compositor_blit_drag_overlap(void) {
    const compositor_snapshot_t *snapshot = &g_compositor_snapshot;
    int64_t local_left;
    int64_t local_top;
    int64_t local_right;
    int64_t local_bottom;
    uint32_t width;
    uint32_t height;

    if (!snapshot->drag_blit_valid ||
        snapshot->dragging_identifier == 0U ||
        snapshot->drag_width == 0U ||
        snapshot->drag_height == 0U ||
        g_window_server.composite_framebuffer == 0 ||
        g_window_server.composite_framebuffer ==
            g_window_server.framebuffer ||
        snapshot->window_count == 0U ||
        snapshot->windows[snapshot->window_count - 1U].identifier !=
            snapshot->dragging_identifier) {
        return;
    }

    local_left = 0;
    local_top = 0;
    local_right = snapshot->drag_width;
    local_bottom = snapshot->drag_height;

    if (snapshot->drag_old_x < 0 &&
        local_left < -(int64_t)snapshot->drag_old_x) {
        local_left = -(int64_t)snapshot->drag_old_x;
    }
    if (snapshot->drag_new_x < 0 &&
        local_left < -(int64_t)snapshot->drag_new_x) {
        local_left = -(int64_t)snapshot->drag_new_x;
    }
    if (snapshot->drag_old_y < 0 &&
        local_top < -(int64_t)snapshot->drag_old_y) {
        local_top = -(int64_t)snapshot->drag_old_y;
    }
    if (snapshot->drag_new_y < 0 &&
        local_top < -(int64_t)snapshot->drag_new_y) {
        local_top = -(int64_t)snapshot->drag_new_y;
    }

    {
        int64_t old_visible_right =
            (int64_t)g_window_server.display_width -
            snapshot->drag_old_x;
        int64_t new_visible_right =
            (int64_t)g_window_server.display_width -
            snapshot->drag_new_x;
        int64_t old_visible_bottom =
            (int64_t)g_window_server.display_height -
            snapshot->drag_old_y;
        int64_t new_visible_bottom =
            (int64_t)g_window_server.display_height -
            snapshot->drag_new_y;

        if (local_right > old_visible_right) {
            local_right = old_visible_right;
        }
        if (local_right > new_visible_right) {
            local_right = new_visible_right;
        }
        if (local_bottom > old_visible_bottom) {
            local_bottom = old_visible_bottom;
        }
        if (local_bottom > new_visible_bottom) {
            local_bottom = new_visible_bottom;
        }
    }

    if (local_left < 0) local_left = 0;
    if (local_top < 0) local_top = 0;

    if (local_left >= local_right ||
        local_top >= local_bottom) {
        return;
    }

    width = (uint32_t)(local_right - local_left);
    height = (uint32_t)(local_bottom - local_top);

    for (uint32_t row_index = 0U;
         row_index < height;
         ++row_index) {
        uint32_t row =
            snapshot->drag_new_y > snapshot->drag_old_y ?
                height - 1U - row_index :
                row_index;

        int64_t local_y = local_top + (int64_t)row;
        int64_t source_y =
            (int64_t)snapshot->drag_old_y + local_y;
        int64_t destination_y =
            (int64_t)snapshot->drag_new_y + local_y;

        int64_t source_x =
            (int64_t)snapshot->drag_old_x + local_left;
        int64_t destination_x =
            (int64_t)snapshot->drag_new_x + local_left;

        /* The fallback path never calls this function, so the retained
         * framebuffer is private WB RAM rather than the visible scanout. */
        uint32_t *scene =
            (uint32_t *)(uintptr_t)
                g_window_server.composite_framebuffer;

        const uint32_t *source =
            scene +
            (uint64_t)source_y *
                g_window_server.display_stride +
            (uint32_t)source_x;

        uint32_t *destination =
            scene +
            (uint64_t)destination_y *
                g_window_server.display_stride +
            (uint32_t)destination_x;

        compositor_move_wb_pixels(
            destination,
            source,
            width);
    }
}
