#include "internal.h"

/* REFACTOR_P7I_SNAPSHOT_DAMAGE_OWNER: immutable damage and tile conversion. */

/* A drag is one visible transaction. */
void compositor_collapse_drag_damage(compositor_snapshot_t *snapshot) {
    uint32_t left;
    uint32_t top;
    uint32_t right;
    uint32_t bottom;

    if (snapshot == 0 || snapshot->dragging_identifier == 0U ||
        snapshot->damage_count <= 1U) {
        return;
    }

    left = (uint32_t)snapshot->damage_rects[0].x0;
    top = (uint32_t)snapshot->damage_rects[0].y0;
    right = (uint32_t)snapshot->damage_rects[0].x1;
    bottom = (uint32_t)snapshot->damage_rects[0].y1;

    for (uint32_t index = 1U;
         index < snapshot->damage_count;
         ++index) {
        const Rect *rect =
            &snapshot->damage_rects[index];

        if ((uint32_t)rect->x0 < left)
            left = (uint32_t)rect->x0;
        if ((uint32_t)rect->y0 < top)
            top = (uint32_t)rect->y0;
        if ((uint32_t)rect->x1 > right)
            right = (uint32_t)rect->x1;
        if ((uint32_t)rect->y1 > bottom)
            bottom = (uint32_t)rect->y1;
    }

    snapshot->damage_rects[0] = (Rect){
        .x0 = left,
        .y0 = top,
        .x1 = right,
        .y1 = bottom,
    };
    snapshot->damage_count = 1U;
}

bool compositor_view_intersects_snapshot_damage(
    const compositor_window_view_t *window,
    const compositor_snapshot_t *snapshot) {

    int64_t left;
    int64_t top;
    int64_t right;
    int64_t bottom;

    if (window == 0 ||
        snapshot == 0 ||
        snapshot->damage_count == 0U) {
        return false;
    }

    left = window->x;
    top = window->y;

    right =
        left +
        (int64_t)window_outer_width(
            window->width,
            window->flags);

    bottom =
        top +
        (int64_t)window_outer_height(
            window->height,
            window->flags);

    if (left >= right || top >= bottom) {
        return false;
    }

    for (uint32_t index = 0U;
         index < snapshot->damage_count;
         ++index) {

        const Rect *damage =
            &snapshot->damage_rects[index];

        if (damage->x0 >= damage->x1 ||
            damage->y0 >= damage->y1) {
            continue;
        }

        if (right <= (int64_t)damage->x0 ||
            bottom <= (int64_t)damage->y0 ||
            left >= (int64_t)damage->x1 ||
            top >= (int64_t)damage->y1) {
            continue;
        }

        return true;
    }

    return false;
}

bool compositor_snapshot_damage_bounds(
    const compositor_snapshot_t *snapshot,
    Rect *bounds) {

    bool have_bounds = false;

    if (snapshot == 0 || bounds == 0) {
        return false;
    }

    for (uint32_t index = 0U;
         index < snapshot->damage_count;
         ++index) {

        const Rect *rect =
            &snapshot->damage_rects[index];

        if (rect->x0 >= rect->x1 ||
            rect->y0 >= rect->y1) {
            continue;
        }

        if (!have_bounds) {
            *bounds = *rect;
            have_bounds = true;
            continue;
        }

        if (rect->x0 < bounds->x0)
            bounds->x0 = rect->x0;
        if (rect->y0 < bounds->y0)
            bounds->y0 = rect->y0;
        if (rect->x1 > bounds->x1)
            bounds->x1 = rect->x1;
        if (rect->y1 > bounds->y1)
            bounds->y1 = rect->y1;
    }

    return have_bounds;
}

uint32_t compositor_snapshot_occlusion_floor(
    const compositor_snapshot_t *snapshot) {

    Rect bounds;
    uint32_t floor = 0U;

    if (snapshot == 0 ||
        !compositor_snapshot_damage_bounds(
            snapshot,
            &bounds)) {
        return 0U;
    }

    for (uint32_t count = snapshot->window_count;
         count != 0U;
         --count) {

        uint32_t index = count - 1U;

        if (compositor_view_fully_covers_snapshot_bounds(
                &snapshot->windows[index],
                &bounds)) {
            floor = index;
            break;
        }
    }

    if (snapshot->dragging_identifier != 0U &&
        floor != 0U) {

        for (uint32_t index = 0U;
             index < floor;
             ++index) {

            if (snapshot->windows[index].identifier ==
                snapshot->dragging_identifier) {
                floor = index;
                break;
            }
        }
    }

    return floor;
}

bool compositor_registry_window_intersects_damage_bounds_locked(
    const window_server_window_t *window) {

    int64_t left;
    int64_t top;
    int64_t right;
    int64_t bottom;

    if (window == 0) {
        return false;
    }

    left = window->x;
    top = window->y;

    right =
        left +
        (int64_t)window_outer_width(
            window->width,
            window->flags);

    bottom =
        top +
        (int64_t)window_outer_height(
            window->height,
            window->flags);

    return
        left < (int64_t)g_window_server.damage_bounds.x1 &&
        right > (int64_t)g_window_server.damage_bounds.x0 &&
        top < (int64_t)g_window_server.damage_bounds.y1 &&
        bottom > (int64_t)g_window_server.damage_bounds.y0;
}

bool compositor_snapshot_damage_tile_is_set(
    const compositor_snapshot_t *snapshot,
    uint32_t tile_x,
    uint32_t tile_y) {
    uint32_t index;

    if (snapshot == 0 ||
        tile_x >= WINDOW_DAMAGE_MAX_TILES_X ||
        tile_y >= WINDOW_DAMAGE_MAX_TILES_Y) {
        return false;
    }

    index = tile_y * WINDOW_DAMAGE_MAX_TILES_X + tile_x;
    return (snapshot->damage_tiles[index >> 6] &
            (1ULL << (index & 63U))) != 0U;
}

uint32_t compositor_snapshot_tiles_to_rects(
    compositor_snapshot_t *snapshot,
    uint32_t capacity) {

    typedef struct active_span {
        uint32_t start_x;
        uint32_t end_x;
        uint32_t rect_index;
    } active_span_t;

    active_span_t previous[
        (WINDOW_DAMAGE_MAX_TILES_X + 1U) / 2U];
    active_span_t current[
        (WINDOW_DAMAGE_MAX_TILES_X + 1U) / 2U];

    uint32_t columns;
    uint32_t rows;
    uint32_t count = 0U;
    uint32_t previous_count = 0U;

    if (snapshot == 0 || capacity == 0U) return 0U;

    columns = window_damage_tile_columns_locked();
    rows = window_damage_tile_rows_locked();

    for (uint32_t tile_y = 0U;
         tile_y < rows;
         ++tile_y) {

        uint32_t tile_x = 0U;
        uint32_t current_count = 0U;
        uint32_t previous_cursor = 0U;

        while (tile_x < columns) {
            uint32_t start_x;
            uint32_t end_x;
            uint32_t rect_index = UINT32_MAX;

            while (tile_x < columns &&
                   !compositor_snapshot_damage_tile_is_set(
                       snapshot,
                       tile_x,
                       tile_y)) {
                ++tile_x;
            }

            if (tile_x >= columns) break;

            start_x = tile_x;

            while (tile_x < columns &&
                   compositor_snapshot_damage_tile_is_set(
                       snapshot,
                       tile_x,
                       tile_y)) {
                ++tile_x;
            }

            end_x = tile_x;

            while (previous_cursor < previous_count &&
                   previous[previous_cursor].start_x < start_x) {
                ++previous_cursor;
            }

            if (previous_cursor < previous_count &&
                previous[previous_cursor].start_x == start_x &&
                previous[previous_cursor].end_x == end_x) {

                uint32_t index =
                    previous[previous_cursor].rect_index;

                if (index < count &&
                    snapshot->damage_rects[index].y1 ==
                        (int32_t)(tile_y * WINDOW_DAMAGE_TILE_SIZE)) {

                    snapshot->damage_rects[index].y1 =
                        (tile_y + 1U) * WINDOW_DAMAGE_TILE_SIZE;

                    if (snapshot->damage_rects[index].y1 >
                        (int32_t)g_window_server.display_height) {
                        snapshot->damage_rects[index].y1 =
                            g_window_server.display_height;
                    }

                    rect_index = index;
                }
            }

            if (rect_index == UINT32_MAX) {
                Rect candidate = {
                    .x0 = start_x * WINDOW_DAMAGE_TILE_SIZE,
                    .y0 = tile_y * WINDOW_DAMAGE_TILE_SIZE,
                    .x1 = end_x * WINDOW_DAMAGE_TILE_SIZE,
                    .y1 = (tile_y + 1U) * WINDOW_DAMAGE_TILE_SIZE,
                };

                if (candidate.x1 >
                    (int32_t)g_window_server.display_width) {
                    candidate.x1 =
                        g_window_server.display_width;
                }

                if (candidate.y1 >
                    (int32_t)g_window_server.display_height) {
                    candidate.y1 =
                        g_window_server.display_height;
                }

                if (count < capacity) {
                    rect_index = count;
                    snapshot->damage_rects[count++] = candidate;
                } else {
                    /* Keep the bounded fallback conservative on overflow. */
                    Rect *tail =
                        &snapshot->damage_rects[capacity - 1U];

                    if (candidate.x0 < tail->x0)
                        tail->x0 = candidate.x0;
                    if (candidate.y0 < tail->y0)
                        tail->y0 = candidate.y0;
                    if (candidate.x1 > tail->x1)
                        tail->x1 = candidate.x1;
                    if (candidate.y1 > tail->y1)
                        tail->y1 = candidate.y1;
                }
            }

            if (rect_index != UINT32_MAX &&
                current_count <
                    (WINDOW_DAMAGE_MAX_TILES_X + 1U) / 2U) {
                current[current_count++] = (active_span_t){
                    .start_x = start_x,
                    .end_x = end_x,
                    .rect_index = rect_index,
                };
            }
        }

        previous_count = current_count;

        for (uint32_t index = 0U;
             index < current_count;
             ++index) {
            previous[index] = current[index];
        }
    }

    return count;
}
