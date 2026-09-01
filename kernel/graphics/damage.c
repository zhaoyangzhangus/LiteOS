#include "internal.h"

#define WINDOW_CURSOR_WIDTH 24U
#define WINDOW_CURSOR_HEIGHT 24U
#define WINDOW_CURSOR_HOTSPOT_X 3U
#define WINDOW_CURSOR_HOTSPOT_Y 1U
uint32_t window_damage_tile_columns_locked(void) {
    uint32_t count = (g_window_server.display_width +
                      WINDOW_DAMAGE_TILE_SIZE - 1U) /
                     WINDOW_DAMAGE_TILE_SIZE;
    return count > WINDOW_DAMAGE_MAX_TILES_X ?
           WINDOW_DAMAGE_MAX_TILES_X : count;
}

uint32_t window_damage_tile_rows_locked(void) {
    uint32_t count = (g_window_server.display_height +
                      WINDOW_DAMAGE_TILE_SIZE - 1U) /
                     WINDOW_DAMAGE_TILE_SIZE;
    return count > WINDOW_DAMAGE_MAX_TILES_Y ?
           WINDOW_DAMAGE_MAX_TILES_Y : count;
}

bool window_damage_tile_is_set_locked(uint32_t tile_x,
                                             uint32_t tile_y) {
    uint32_t index = tile_y * WINDOW_DAMAGE_MAX_TILES_X + tile_x;
    return (g_window_server.damage_tiles[index >> 6] &
            (1ULL << (index & 63U))) != 0U;
}

static void window_damage_tile_range_set_locked(
    uint32_t tile_y,
    uint32_t first_x,
    uint32_t last_x) {

    uint32_t first_index;
    uint32_t last_index;
    uint32_t first_word;
    uint32_t last_word;
    uint32_t first_bit;
    uint32_t last_bit;
    uint64_t first_mask;
    uint64_t last_mask;

    if (first_x > last_x ||
        tile_y >= WINDOW_DAMAGE_MAX_TILES_Y ||
        last_x >= WINDOW_DAMAGE_MAX_TILES_X) {
        return;
    }

    first_index =
        tile_y * WINDOW_DAMAGE_MAX_TILES_X + first_x;

    last_index =
        tile_y * WINDOW_DAMAGE_MAX_TILES_X + last_x;

    first_word = first_index >> 6;
    last_word = last_index >> 6;

    first_bit = first_index & 63U;
    last_bit = last_index & 63U;

    first_mask = UINT64_MAX << first_bit;

    last_mask =
        last_bit == 63U ?
            UINT64_MAX :
            ((1ULL << (last_bit + 1U)) - 1ULL);

    if (first_word == last_word) {
        g_window_server.damage_tiles[first_word] |=
            first_mask & last_mask;
        return;
    }

    g_window_server.damage_tiles[first_word] |= first_mask;

    for (uint32_t word = first_word + 1U;
         word < last_word;
         ++word) {
        g_window_server.damage_tiles[word] = UINT64_MAX;
    }

    g_window_server.damage_tiles[last_word] |= last_mask;
}

static void window_damage_tiles_clear_locked(void) {
    for (uint32_t word = 0U; word < WINDOW_DAMAGE_TILE_WORDS; ++word) {
        g_window_server.damage_tiles[word] = 0U;
    }
}

/* Damage storage is initialized before the first display mode is known. */
void window_damage_reset_locked(void) {
    g_window_server.dirty = false;
    g_window_server.damage_full = false;
    g_window_server.damage_tiles_active = false;
    g_window_server.damage_count = 0U;
    g_window_server.damage_bounds.x0 = 0U;
    g_window_server.damage_bounds.y0 = 0U;
    g_window_server.damage_bounds.x1 = 0U;
    g_window_server.damage_bounds.y1 = 0U;
    window_damage_tiles_clear_locked();
    atomic_store_explicit(&g_window_dirty_hint, false,
                          memory_order_release);
}

/* Discard an empty producer generation without touching compositor state. */
void window_damage_clear_pending_locked(void) {
    g_window_server.dirty = false;
    atomic_store_explicit(&g_window_dirty_hint, false,
                          memory_order_release);
}

/* Rotate producer-owned damage into the immutable compositor snapshot. */
void window_damage_rotate_locked(void) {
    g_window_server.damage_count = 0U;
    g_window_server.damage_full = false;
    g_window_server.damage_tiles_active = false;
    window_damage_clear_pending_locked();
    g_window_server.damage_bounds.x0 = g_window_server.display_width;
    g_window_server.damage_bounds.y0 = g_window_server.display_height;
    g_window_server.damage_bounds.x1 = 0U;
    g_window_server.damage_bounds.y1 = 0U;
}

static void window_damage_mark_tiles_locked(const Rect *rect) {
    uint32_t columns;
    uint32_t rows;
    uint32_t first_x;
    uint32_t first_y;
    uint32_t last_x;
    uint32_t last_y;
    if (rect == 0 ||
        rect->x0 >= rect->x1 ||
        rect->y0 >= rect->y1) {
        return;
    }

    columns = window_damage_tile_columns_locked();
    rows = window_damage_tile_rows_locked();
    if (columns == 0U || rows == 0U) return;
    first_x = rect->x0 / WINDOW_DAMAGE_TILE_SIZE;
    first_y = rect->y0 / WINDOW_DAMAGE_TILE_SIZE;
    last_x = (rect->x1 - 1U) / WINDOW_DAMAGE_TILE_SIZE;
    last_y = (rect->y1 - 1U) / WINDOW_DAMAGE_TILE_SIZE;
    if (first_x >= columns || first_y >= rows) return;
    if (last_x >= columns) last_x = columns - 1U;
    if (last_y >= rows) last_y = rows - 1U;
    for (uint32_t tile_y = first_y;
         tile_y <= last_y;
         ++tile_y) {
        window_damage_tile_range_set_locked(
            tile_y, first_x, last_x);
    }
}

/*
 * Publish one NEW producer generation.
 *
 * window_lock is held, so g_window_server.dirty is the edge detector.
 * Return true only for clean -> dirty.
 */
static inline bool window_dirty_edge_locked(void) {
    if (g_window_server.dirty) {
        return false;
    }

    g_window_server.dirty = true;

    /*
     * Generation metadata is read outside window_lock. dirty_hint release is
     * the publication point for the new pending scene generation.
     */
    (void)atomic_fetch_add_explicit(
        &g_window_dirty_generation,
        1U,
        memory_order_relaxed);

    atomic_store_explicit(
        &g_window_dirty_hint,
        true,
        memory_order_release);

    return true;
}


void window_mark_dirty_locked(void);

static void window_damage_enable_tiles_locked(void) {
    if (g_window_server.damage_tiles_active) return;
    /* The fixed bitmap intentionally covers normal 4K-class outputs.  If a
     * future mode exceeds it, retain correctness with the existing full
     * damage path instead of silently clipping the right/bottom edge. */
    if (g_window_server.display_width >
            WINDOW_DAMAGE_MAX_TILES_X * WINDOW_DAMAGE_TILE_SIZE ||
        g_window_server.display_height >
            WINDOW_DAMAGE_MAX_TILES_Y * WINDOW_DAMAGE_TILE_SIZE) {
        window_mark_dirty_locked();
        return;
    }
    window_damage_tiles_clear_locked();
    for (uint32_t index = 0U;
         index < g_window_server.damage_count;
         ++index) {
        window_damage_mark_tiles_locked(&g_window_server.damage_rects[index]);
    }
    g_window_server.damage_count = 0U;
    g_window_server.damage_tiles_active = true;
}

void window_mark_dirty_locked(void) {
    /*
     * Escalating an already-dirty generation to full damage does not create a
     * second scheduling edge.
     */
    (void)window_dirty_edge_locked();

    g_window_server.damage_full = true;
    g_window_server.damage_tiles_active = false;
    g_window_server.damage_count = 0U;
    g_window_server.damage_bounds.x0 = 0U;
    g_window_server.damage_bounds.y0 = 0U;
    g_window_server.damage_bounds.x1 = g_window_server.display_width;
    g_window_server.damage_bounds.y1 = g_window_server.display_height;
}

void window_mark_rect_locked(int32_t x, int32_t y,
                                    uint32_t width, uint32_t height) {
    int64_t right = (int64_t)x + width;
    int64_t bottom = (int64_t)y + height;
    uint32_t left;
    uint32_t top;
    if (width == 0U || height == 0U || g_window_server.display_width == 0U ||
        g_window_server.display_height == 0U || right <= 0 || bottom <= 0 ||
        x >= (int32_t)g_window_server.display_width ||
        y >= (int32_t)g_window_server.display_height) return;
    left = x < 0 ? 0U : (uint32_t)x;
    top = y < 0 ? 0U : (uint32_t)y;
    if (right > (int64_t)g_window_server.display_width) {
        right = g_window_server.display_width;
    }
    if (bottom > (int64_t)g_window_server.display_height) {
        bottom = g_window_server.display_height;
    }
    bool first_damage =
        !g_window_server.dirty;

    if (first_damage) {
        (void)window_dirty_edge_locked();
    }

    if (g_window_server.damage_full) {
        return;
    }

    Rect candidate = {
        .x0 = left,
        .y0 = top,
        .x1 = (uint32_t)right,
        .y1 = (uint32_t)bottom,
    };

    /*
     * One conservative producer bound lets snapshot capture reject unrelated
     * windows with four comparisons, without expanding tile damage or scanning
     * the full rectangle set under window_lock.
     */
    if (first_damage) {
        g_window_server.damage_bounds.x0 =
            candidate.x0;
        g_window_server.damage_bounds.y0 =
            candidate.y0;
        g_window_server.damage_bounds.x1 =
            candidate.x1;
        g_window_server.damage_bounds.y1 =
            candidate.y1;
    } else {
        if (candidate.x0 <
            g_window_server.damage_bounds.x0) {
            g_window_server.damage_bounds.x0 =
                candidate.x0;
        }

        if (candidate.y0 <
            g_window_server.damage_bounds.y0) {
            g_window_server.damage_bounds.y0 =
                candidate.y0;
        }

        if (candidate.x1 >
            g_window_server.damage_bounds.x1) {
            g_window_server.damage_bounds.x1 =
                candidate.x1;
        }

        if (candidate.y1 >
            g_window_server.damage_bounds.y1) {
            g_window_server.damage_bounds.y1 =
                candidate.y1;
        }
    }
    if (g_window_server.damage_tiles_active) {
        window_damage_mark_tiles_locked(&candidate);
        return;
    }
    for (uint32_t index = 0U; index < g_window_server.damage_count; ++index) {
        Rect *current = &g_window_server.damage_rects[index];
        uint32_t merged_left;
        uint32_t merged_top;
        uint32_t merged_right;
        uint32_t merged_bottom;
        uint32_t overlap_left;
        uint32_t overlap_top;
        uint32_t overlap_right;
        uint32_t overlap_bottom;
        uint64_t current_area;
        uint64_t candidate_area;
        uint64_t overlap_area = 0U;
        uint64_t union_area;
        uint64_t merged_area;

        if (candidate.x1 < current->x0 ||
            current->x1 < candidate.x0 ||
            candidate.y1 < current->y0 ||
            current->y1 < candidate.y0) {
            continue;
        }

        merged_left = candidate.x0 < current->x0 ?
                      candidate.x0 : current->x0;
        merged_top = candidate.y0 < current->y0 ?
                     candidate.y0 : current->y0;
        merged_right = candidate.x1 > current->x1 ?
                       candidate.x1 : current->x1;
        merged_bottom = candidate.y1 > current->y1 ?
                        candidate.y1 : current->y1;

        overlap_left = candidate.x0 > current->x0 ?
                       candidate.x0 : current->x0;
        overlap_top = candidate.y0 > current->y0 ?
                      candidate.y0 : current->y0;
        overlap_right = candidate.x1 < current->x1 ?
                        candidate.x1 : current->x1;
        overlap_bottom = candidate.y1 < current->y1 ?
                         candidate.y1 : current->y1;

        current_area =
            (uint64_t)(current->x1 - current->x0) *
            (current->y1 - current->y0);
        candidate_area =
            (uint64_t)(candidate.x1 - candidate.x0) *
            (candidate.y1 - candidate.y0);

        if (overlap_left < overlap_right &&
            overlap_top < overlap_bottom) {
            overlap_area =
                (uint64_t)(overlap_right - overlap_left) *
                (overlap_bottom - overlap_top);
        }

        union_area = current_area + candidate_area - overlap_area;

        merged_area =
            (uint64_t)(merged_right - merged_left) *
            (merged_bottom - merged_top);

        /*
         * Bounding-box merge is only worthwhile when it wastes at most 25%
         * compared with the real union.  This prevents a diagonal chain of
         * small mouse damages from gradually becoming one huge rectangle.
         */
        if (merged_area > union_area + union_area / 4U) {
            continue;
        }

        current->x0 = merged_left;
        current->y0 = merged_top;
        current->x1 = merged_right;
        current->y1 = merged_bottom;
        return;
    }
    if (g_window_server.damage_count >= WINDOW_DAMAGE_MAX_RECTS) {
        window_damage_enable_tiles_locked();
        window_damage_mark_tiles_locked(&candidate);
        return;
    }
    g_window_server.damage_rects[g_window_server.damage_count++] = candidate;
}

void window_mark_window_locked(const window_server_window_t *window) {
    if (window == 0) return;
    window_mark_rect_locked(window->x, window->y,
                            window_outer_width(window->width, window->flags),
                            window_outer_height(window->height, window->flags));
}

void window_mark_surface_locked(const window_server_window_t *window,
                                       int32_t x, int32_t y,
                                       uint32_t width, uint32_t height) {
    int64_t left;
    int64_t top;
    int64_t right;
    int64_t bottom;
    if (window == 0 || width == 0U || height == 0U) return;
    left = (int64_t)window->x +
           window_client_offset_x(window->flags) + x;
    top =
        (int64_t)window->y +
        window_client_offset_y(window->flags) +
        y;
    right = left + width;
    bottom = top + height;
    if (right <= 0 || bottom <= 0 ||
        left >= (int64_t)g_window_server.display_width ||
        top >= (int64_t)g_window_server.display_height) return;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > (int64_t)g_window_server.display_width) {
        right = g_window_server.display_width;
    }
    if (bottom > (int64_t)g_window_server.display_height) {
        bottom = g_window_server.display_height;
    }
    if (left >= right || top >= bottom) return;
    window_mark_rect_locked((int32_t)left, (int32_t)top,
                            (uint32_t)(right - left),
                            (uint32_t)(bottom - top));
}

/*
 * Window movement produces two logical damages:
 *
 *   1. old position: restore whatever is now underneath the window
 *   2. new position: draw the window at its new coordinates
 *
 * Do not immediately convert them into one bounding rectangle.  The generic
 * damage merge code already combines nearby/overlapping rectangles when the
 * bounding box wastes <=25%, while large moves remain as two independent
 * regions.
 *
 * For a topmost drag the overlapping pixels are moved in the retained
 * composite buffer, so only the exposed strips need rasterization.  Other
 * callers request the complete new rectangle with render_new_position=true.
 */
void window_mark_moved_rect_locked(
    int32_t old_x,
    int32_t old_y,
    int32_t new_x,
    int32_t new_y,
    uint32_t width,
    uint32_t height,
    bool render_new_position) {

    int64_t old_left;
    int64_t old_top;
    int64_t old_right;
    int64_t old_bottom;

    int64_t new_left;
    int64_t new_top;
    int64_t new_right;
    int64_t new_bottom;

    int64_t overlap_left;
    int64_t overlap_top;
    int64_t overlap_right;
    int64_t overlap_bottom;

    if (width == 0U ||
        height == 0U) {
        return;
    }

    if (old_x == new_x &&
        old_y == new_y) {

        window_mark_rect_locked(
            old_x,
            old_y,
            width,
            height);

        return;
    }

    old_left = old_x;
    old_top = old_y;
    old_right =
        old_left + (int64_t)width;
    old_bottom =
        old_top + (int64_t)height;

    new_left = new_x;
    new_top = new_y;
    new_right =
        new_left + (int64_t)width;
    new_bottom =
        new_top + (int64_t)height;

    overlap_left =
        old_left > new_left ?
        old_left : new_left;

    overlap_top =
        old_top > new_top ?
        old_top : new_top;

    overlap_right =
        old_right < new_right ?
        old_right : new_right;

    overlap_bottom =
        old_bottom < new_bottom ?
        old_bottom : new_bottom;

    /*
     * No overlap: retain the normal old/new pair.
     */
    if (overlap_left >= overlap_right ||
        overlap_top >= overlap_bottom) {

        window_mark_rect_locked(
            old_x,
            old_y,
            width,
            height);

        window_mark_rect_locked(
            new_x,
            new_y,
            width,
            height);

        return;
    }

    /*
     * OLD \ NEW:
     *
     * horizontal exposed strip across full old height.
     */
    if (new_left > old_left) {
        uint32_t exposed_width =
            (uint32_t)(
                overlap_left -
                old_left);

        if (exposed_width != 0U) {
            window_mark_rect_locked(
                old_x,
                old_y,
                exposed_width,
                height);
        }
    } else if (new_left < old_left) {
        uint32_t exposed_width =
            (uint32_t)(
                old_right -
                overlap_right);

        if (exposed_width != 0U) {
            window_mark_rect_locked(
                (int32_t)overlap_right,
                old_y,
                exposed_width,
                height);
        }
    }

    /*
     * Vertical exposed strip only covers horizontal overlap, so it cannot
     * overlap the strip above.
     */
    {
        uint32_t overlap_width =
            (uint32_t)(
                overlap_right -
                overlap_left);

        if (new_top > old_top) {
            uint32_t exposed_height =
                (uint32_t)(
                    overlap_top -
                    old_top);

            if (overlap_width != 0U &&
                exposed_height != 0U) {

                window_mark_rect_locked(
                    (int32_t)overlap_left,
                    old_y,
                    overlap_width,
                    exposed_height);
            }
        } else if (new_top < old_top) {
            uint32_t exposed_height =
                (uint32_t)(
                    old_bottom -
                    overlap_bottom);

            if (overlap_width != 0U &&
                exposed_height != 0U) {

                window_mark_rect_locked(
                    (int32_t)overlap_left,
                    (int32_t)overlap_bottom,
                    overlap_width,
                    exposed_height);
            }
        }
    }

    if (!render_new_position) {
        /* NEW \ OLD: the overlap has already been copied by the retained
         * drag blit, so render only the newly uncovered horizontal/vertical
         * strips at the destination position. */
        if (old_left > new_left) {
            uint32_t exposed_width =
                (uint32_t)(overlap_left - new_left);
            if (exposed_width != 0U) {
                window_mark_rect_locked(new_x, new_y,
                                        exposed_width, height);
            }
        } else if (old_left < new_left) {
            uint32_t exposed_width =
                (uint32_t)(new_right - overlap_right);
            if (exposed_width != 0U) {
                window_mark_rect_locked((int32_t)overlap_right, new_y,
                                        exposed_width, height);
            }
        }

        {
            uint32_t overlap_width =
                (uint32_t)(overlap_right - overlap_left);
            if (old_top > new_top) {
                uint32_t exposed_height =
                    (uint32_t)(overlap_top - new_top);
                if (overlap_width != 0U && exposed_height != 0U) {
                    window_mark_rect_locked((int32_t)overlap_left, new_y,
                                            overlap_width, exposed_height);
                }
            } else if (old_top < new_top) {
                uint32_t exposed_height =
                    (uint32_t)(new_bottom - overlap_bottom);
                if (overlap_width != 0U && exposed_height != 0U) {
                    window_mark_rect_locked((int32_t)overlap_left,
                                            (int32_t)overlap_bottom,
                                            overlap_width, exposed_height);
                }
            }
        }
    }

    if (render_new_position) {
        /* Without a retained-pixel move, the complete final position must
         * still be rendered. */
        window_mark_rect_locked(new_x, new_y, width, height);
    }
}

void window_mark_drag_corner_repair_locked(
    int32_t x,
    int32_t y,
    uint32_t outer_width,
    uint32_t outer_height,
    uint32_t flags) {

    uint32_t radius = WINDOW_CORNER_RADIUS;

    (void)flags;

    if (outer_width == 0U || outer_height == 0U) {
        return;
    }

    if (radius > outer_width) radius = outer_width;
    if (radius > outer_height) radius = outer_height;
    if (radius == 0U) return;

    window_mark_rect_locked(x, y, radius, radius);
    window_mark_rect_locked(
        x + (int32_t)(outer_width - radius),
        y, radius, radius);
    window_mark_rect_locked(
        x,
        y + (int32_t)(outer_height - radius),
        radius, radius);
    window_mark_rect_locked(
        x + (int32_t)(outer_width - radius),
        y + (int32_t)(outer_height - radius),
        radius, radius);
}


void window_mark_moved_cursor_locked(uint32_t old_x, uint32_t old_y,
                                            uint32_t new_x, uint32_t new_y) {
    if (old_x == new_x && old_y == new_y) {
        return;
    }

    if (g_window_server.composite_framebuffer != 0 &&
        g_window_server.composite_framebuffer !=
            g_window_server.framebuffer) {
        /*
         * The retained WB scene deliberately excludes the cursor.  Moving
         * only the pointer does not change scene pixels and must not schedule
         * a full compositor frame.
         */
        return;
    }

    int32_t old_left;
    int32_t old_top;
    int32_t new_left;
    int32_t new_top;

    /* pointer_x/y 鏄?cursor hotspot锛屼笉鏄?sprite 宸︿笂瑙掋€?*/
    old_left = (int32_t)old_x - (int32_t)WINDOW_CURSOR_HOTSPOT_X;
    old_top = (int32_t)old_y - (int32_t)WINDOW_CURSOR_HOTSPOT_Y;
    new_left = (int32_t)new_x - (int32_t)WINDOW_CURSOR_HOTSPOT_X;
    new_top = (int32_t)new_y - (int32_t)WINDOW_CURSOR_HOTSPOT_Y;

    /*
     * Old/new cursor locations are independent damages.  Do not create a
     * potentially screen-sized bounding box when the pointer jumps.
     */
    window_mark_rect_locked(old_left, old_top,
                            WINDOW_CURSOR_WIDTH, WINDOW_CURSOR_HEIGHT);

    if (old_left != new_left || old_top != new_top) {
        window_mark_rect_locked(new_left, new_top,
                                WINDOW_CURSOR_WIDTH, WINDOW_CURSOR_HEIGHT);
    }
}

/* 涓€娆￠紶鏍囦簨鍔″彧鍏佽涓€娆?framebuffer 鍖哄煙鎻愪氦銆傜劍鐐广€佷换鍔℃爮銆佽鎷栧姩
 * 绐楀彛鍜屽厜鏍囧彲鑳藉垎鍒骇鐢?damage锛涘垎寮€鎻愪氦浼氳 GOP 鎵弿鍒板崐甯х姸鎬併€?*/
void window_coalesce_damage_locked(void) {
    uint32_t first = 0U;

    if (g_window_server.damage_full ||
        g_window_server.damage_tiles_active ||
        g_window_server.damage_count <= 1U) {
        return;
    }

    /*
     * Keep independent damage rectangles independent.  Merge a pair only
     * when the bounding rectangle adds <=25% wasted pixels over their real
     * union.  At most WINDOW_DAMAGE_MAX_RECTS exist, so this O(n^2) pass is
     * tiny compared with repainting a needlessly large framebuffer region.
     */
    while (first < g_window_server.damage_count) {
        uint32_t second = first + 1U;
        bool merged_any = false;

        while (second < g_window_server.damage_count) {
            Rect *a =
                &g_window_server.damage_rects[first];
            const Rect *b =
                &g_window_server.damage_rects[second];

            uint32_t left = a->x0 < b->x0 ? a->x0 : b->x0;
            uint32_t top = a->y0 < b->y0 ? a->y0 : b->y0;
            uint32_t right = a->x1 > b->x1 ? a->x1 : b->x1;
            uint32_t bottom =
                a->y1 > b->y1 ? a->y1 : b->y1;

            uint32_t overlap_left =
                a->x0 > b->x0 ? a->x0 : b->x0;
            uint32_t overlap_top =
                a->y0 > b->y0 ? a->y0 : b->y0;
            uint32_t overlap_right =
                a->x1 < b->x1 ? a->x1 : b->x1;
            uint32_t overlap_bottom =
                a->y1 < b->y1 ? a->y1 : b->y1;

            uint64_t area_a =
                (uint64_t)(a->x1 - a->x0) *
                (a->y1 - a->y0);
            uint64_t area_b =
                (uint64_t)(b->x1 - b->x0) *
                (b->y1 - b->y0);
            uint64_t overlap_area = 0U;
            uint64_t union_area;
            uint64_t merged_area;

            if (overlap_left < overlap_right &&
                overlap_top < overlap_bottom) {
                overlap_area =
                    (uint64_t)(overlap_right - overlap_left) *
                    (overlap_bottom - overlap_top);
            }

            union_area = area_a + area_b - overlap_area;
            merged_area =
                (uint64_t)(right - left) *
                (bottom - top);

            if (merged_area <= union_area + union_area / 4U) {
                a->x0 = left;
                a->y0 = top;
                a->x1 = right;
                a->y1 = bottom;

                for (uint32_t index = second + 1U;
                     index < g_window_server.damage_count;
                     ++index) {
                    g_window_server.damage_rects[index - 1U] =
                        g_window_server.damage_rects[index];
                }

                --g_window_server.damage_count;
                merged_any = true;

                /*
                 * a changed, so retry it against every remaining rectangle.
                 */
                second = first + 1U;
                continue;
            }

            ++second;
        }

        if (!merged_any) {
            ++first;
        } else {
            /*
             * No pair with this newly expanded rectangle remains mergeable
             * after the restarted scan.
             */
            ++first;
        }
    }
}

/*
 * 鍦嗚鍗婂緞鍥哄畾涓?WINDOW_CORNER_RADIUS锛屽洜姝や笉鍦ㄥ悎鎴愮儹璺緞涓绠楀钩鏂瑰拰銆? * 姣忎竴椤硅〃绀鸿鎵弿绾夸粠宸︺€佸彸鍚勮鎺夌殑鍍忕礌鏁帮紱涓嬪崐閮ㄥ垎鍙嶅悜绱㈠紩鍗冲彲銆? */
