#include "internal.h"

/* REFACTOR_P7H_RENDER_PLAN_OWNER: immutable spatial plan compilation. */

compositor_render_span_t
    g_compositor_render_plan[WINDOW_RENDER_PLAN_MAX_SPANS];
uint32_t g_compositor_render_plan_count;
bool g_compositor_render_plan_valid;

void compositor_render_plan_reset(void) {
    g_compositor_render_plan_count = 0U;
    g_compositor_render_plan_valid = false;
}

bool compositor_view_fully_covers_snapshot_bounds(
    const compositor_window_view_t *window,
    const Rect *bounds) {

    int64_t frame_left;
    int64_t frame_top;
    int64_t frame_right;
    int64_t frame_bottom;

    int64_t opaque_left;
    int64_t opaque_top;
    int64_t opaque_right;
    int64_t opaque_bottom;

    uint64_t frame_width;
    uint64_t frame_height;

    if (window == 0 || bounds == 0) {
        return false;
    }

    frame_width =
        (uint64_t)window_outer_width(
            window->width,
            window->flags);

    frame_height =
        (uint64_t)window_outer_height(
            window->height,
            window->flags);

    if (frame_width <= WINDOW_CORNER_RADIUS * 2U ||
        frame_height <= WINDOW_CORNER_RADIUS * 2U) {
        return false;
    }

    frame_left = window->x;
    frame_top = window->y;
    frame_right = frame_left + (int64_t)frame_width;
    frame_bottom = frame_top + (int64_t)frame_height;

    opaque_left = frame_left + WINDOW_CORNER_RADIUS;
    opaque_top = frame_top + WINDOW_CORNER_RADIUS;
    opaque_right = frame_right - WINDOW_CORNER_RADIUS;
    opaque_bottom = frame_bottom - WINDOW_CORNER_RADIUS;

    return
        (int64_t)bounds->x0 >= opaque_left &&
        (int64_t)bounds->y0 >= opaque_top &&
        (int64_t)bounds->x1 <= opaque_right &&
        (int64_t)bounds->y1 <= opaque_bottom;
}

static uint32_t compositor_render_plan_exact_floor(
    const compositor_snapshot_t *snapshot,
    const Rect *rect) {

    if (snapshot == 0 || rect == 0 ||
        rect->x0 >= rect->x1 ||
        rect->y0 >= rect->y1) {
        return UINT32_MAX;
    }

    for (uint32_t count = snapshot->window_count;
         count != 0U;
         --count) {

        uint32_t index = count - 1U;

        if (compositor_view_fully_covers_snapshot_bounds(
                &snapshot->windows[index],
                rect)) {
            return index;
        }
    }

    return UINT32_MAX;
}

static uint32_t compositor_render_plan_tile_floor(
    const compositor_snapshot_t *snapshot,
    uint32_t tile_x,
    uint32_t tile_y) {

    uint32_t columns;
    uint32_t rows;
    uint32_t value;

    if (snapshot == 0) {
        return UINT32_MAX;
    }

    /* P7B TileMeta is the canonical blocker source. */
    const compositor_tile_meta_t *metadata =
        compositor_tile_metadata_at(tile_x, tile_y);

    if (metadata != 0) {
        uint64_t blockers =
            metadata->full_mask & metadata->opaque_mask;

        if (blockers == 0U) return UINT32_MAX;

        value =
            63U -
            (uint32_t)__builtin_clzll(blockers);

        return value < snapshot->window_count ?
            value :
            UINT32_MAX;
    }

    if (!g_compositor_occlusion_floor_valid) {
        return UINT32_MAX;
    }

    columns = window_damage_tile_columns_locked();
    rows = window_damage_tile_rows_locked();

    if (tile_x >= columns || tile_y >= rows) {
        return UINT32_MAX;
    }

    value =
        g_compositor_occlusion_floor[
            tile_y * WINDOW_DAMAGE_MAX_TILES_X +
            tile_x];

    if (value == WINDOW_OCCLUSION_NO_FLOOR ||
        value >= snapshot->window_count) {
        return UINT32_MAX;
    }

    return value;
}

typedef struct compositor_render_active_span {
    uint32_t x0;
    uint32_t x1;
    uint32_t first_window;
    uint32_t plan_index;
} compositor_render_active_span_t;

typedef struct compositor_render_row_span {
    uint32_t x0;
    uint32_t x1;
    uint32_t first_window;
} compositor_render_row_span_t;

static bool compositor_view_intersects_render_rect(
    const compositor_window_view_t *window,
    const Rect *rect) {

    int64_t left;
    int64_t top;
    int64_t right;
    int64_t bottom;

    if (window == 0 || rect == 0 ||
        rect->x0 >= rect->x1 ||
        rect->y0 >= rect->y1) {
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
        right > (int64_t)rect->x0 &&
        bottom > (int64_t)rect->y0 &&
        left < (int64_t)rect->x1 &&
        top < (int64_t)rect->y1;
}

static uint64_t compositor_compile_render_span_mask(
    const compositor_snapshot_t *snapshot,
    const compositor_render_span_t *span) {

    uint64_t mask = 0U;
    uint32_t first;

    if (snapshot == 0 || span == 0 ||
        snapshot->window_count == 0U) {
        return 0U;
    }

    first =
        span->first_window == UINT32_MAX ?
            0U :
            span->first_window;

    if (first >= snapshot->window_count) {
        return 0U;
    }

    /* Single-tile/single-window spans avoid the full immutable-view scan. */
    if (span->rect.x0 >= 0 && span->rect.y0 >= 0 &&
        span->rect.x0 < span->rect.x1 &&
        span->rect.y0 < span->rect.y1) {

        uint32_t first_tile_x =
            (uint32_t)span->rect.x0 /
            WINDOW_DAMAGE_TILE_SIZE;
        uint32_t last_tile_x =
            (uint32_t)(span->rect.x1 - 1) /
            WINDOW_DAMAGE_TILE_SIZE;
        uint32_t first_tile_y =
            (uint32_t)span->rect.y0 /
            WINDOW_DAMAGE_TILE_SIZE;
        uint32_t last_tile_y =
            (uint32_t)(span->rect.y1 - 1) /
            WINDOW_DAMAGE_TILE_SIZE;

        if (first_tile_x == last_tile_x &&
            first_tile_y == last_tile_y) {

            const compositor_tile_meta_t *metadata =
                compositor_tile_metadata_at(
                    first_tile_x,
                    first_tile_y);

            if (metadata != 0 &&
                metadata->touch_mask == 0U) {
                return 0U;
            }

            if (metadata != 0 &&
                (metadata->touch_mask &
                 (metadata->touch_mask - 1U)) == 0U) {

                uint32_t index =
                    (uint32_t)__builtin_ctzll(
                        metadata->touch_mask);

                if (index < first ||
                    index >= snapshot->window_count ||
                    !compositor_view_intersects_render_rect(
                        &snapshot->windows[index],
                        &span->rect)) {
                    return 0U;
                }

                return 1ULL << index;
            }
        }
    }

    for (uint32_t index = first;
         index < snapshot->window_count;
         ++index) {

        if (!compositor_view_intersects_render_rect(
                &snapshot->windows[index],
                &span->rect)) {
            continue;
        }

        mask |= 1ULL << index;
    }

    return mask;
}

static void compositor_compile_render_plan_commands(
    const compositor_snapshot_t *snapshot) {

    if (snapshot == 0 ||
        !g_compositor_render_plan_valid) {
        return;
    }

    for (uint32_t index = 0U;
         index < g_compositor_render_plan_count;
         ++index) {

        compositor_render_span_t *span =
            &g_compositor_render_plan[index];

        span->desktop_required =
            span->first_window == UINT32_MAX;

        span->window_mask =
            compositor_compile_render_span_mask(
                snapshot,
                span);
    }
}

bool compositor_build_render_plan(
    const compositor_snapshot_t *snapshot) {

    compositor_render_active_span_t
        previous[WINDOW_DAMAGE_MAX_TILES_X];

    compositor_render_active_span_t
        current[WINDOW_DAMAGE_MAX_TILES_X];

    compositor_render_row_span_t
        row_spans[WINDOW_DAMAGE_MAX_TILES_X];

    uint32_t columns;
    uint32_t rows;

    g_compositor_render_plan_count = 0U;
    g_compositor_render_plan_valid = false;

    /* Keep the old simple path when tile setup cannot amortize its cost. */
    if (snapshot == 0 || snapshot->damage_count == 0U ||
        (!g_compositor_occlusion_floor_valid &&
         !compositor_tile_metadata_valid())) {
        return false;
    }

    columns = window_damage_tile_columns_locked();
    rows = window_damage_tile_rows_locked();

    if (columns == 0U || rows == 0U) {
        return false;
    }

    for (uint32_t damage_index = 0U;
         damage_index < snapshot->damage_count;
         ++damage_index) {

        const Rect *damage =
            &snapshot->damage_rects[damage_index];

        uint32_t first_tile_x;
        uint32_t last_tile_x;
        uint32_t first_tile_y;
        uint32_t last_tile_y;

        uint32_t previous_count = 0U;

        if (damage->x0 >= damage->x1 ||
            damage->y0 >= damage->y1) {
            continue;
        }

        first_tile_x =
            damage->x0 /
            WINDOW_DAMAGE_TILE_SIZE;

        last_tile_x =
            (damage->x1 - 1U) /
            WINDOW_DAMAGE_TILE_SIZE;

        first_tile_y =
            damage->y0 /
            WINDOW_DAMAGE_TILE_SIZE;

        last_tile_y =
            (damage->y1 - 1U) /
            WINDOW_DAMAGE_TILE_SIZE;

        if (first_tile_x >= columns ||
            first_tile_y >= rows) {
            continue;
        }

        if (last_tile_x >= columns)
            last_tile_x = columns - 1U;

        if (last_tile_y >= rows)
            last_tile_y = rows - 1U;

        for (uint32_t tile_y = first_tile_y;
             tile_y <= last_tile_y;
             ++tile_y) {

            uint32_t row_count = 0U;
            uint32_t current_count = 0U;
            uint32_t previous_cursor = 0U;

            uint32_t tile_x = first_tile_x;

            uint32_t row_top =
                tile_y * WINDOW_DAMAGE_TILE_SIZE;

            uint32_t row_bottom =
                row_top + WINDOW_DAMAGE_TILE_SIZE;

            if (row_top < (uint32_t)damage->y0)
                row_top = (uint32_t)damage->y0;

            if (row_bottom > (uint32_t)damage->y1)
                row_bottom = (uint32_t)damage->y1;

            while (tile_x <= last_tile_x) {
                uint32_t floor =
                    compositor_render_plan_tile_floor(
                        snapshot,
                        tile_x,
                        tile_y);

                uint32_t run_start = tile_x;
                uint32_t run_end = tile_x + 1U;

                if (floor != UINT32_MAX) {
                    while (run_end <= last_tile_x &&
                           compositor_render_plan_tile_floor(
                               snapshot,
                               run_end,
                               tile_y) == floor) {
                        ++run_end;
                    }
                }

                uint32_t left =
                    run_start * WINDOW_DAMAGE_TILE_SIZE;
                uint32_t right =
                    run_end * WINDOW_DAMAGE_TILE_SIZE;

                if (left < (uint32_t)damage->x0)
                    left = (uint32_t)damage->x0;

                if (right > (uint32_t)damage->x1)
                    right = (uint32_t)damage->x1;

                if (left < right && row_top < row_bottom) {
                    if (floor == UINT32_MAX) {
                        Rect exact_rect = {
                            .x0 = left,
                            .y0 = row_top,
                            .x1 = right,
                            .y1 = row_bottom,
                        };

                        floor =
                            compositor_render_plan_exact_floor(
                                snapshot,
                                &exact_rect);
                    }

                    if (row_count != 0U &&
                        row_spans[row_count - 1U].x1 == left &&
                        row_spans[row_count - 1U].first_window == floor) {

                        row_spans[row_count - 1U].x1 = right;
                    } else {
                        if (row_count >= WINDOW_DAMAGE_MAX_TILES_X) {
                            g_compositor_render_plan_count = 0U;
                            return false;
                        }

                        row_spans[row_count++] =
                            (compositor_render_row_span_t){
                                .x0 = left,
                                .x1 = right,
                                .first_window = floor,
                            };
                    }
                }

                tile_x = run_end;
            }

            for (uint32_t row_index = 0U;
                 row_index < row_count;
                 ++row_index) {

                compositor_render_row_span_t *row =
                    &row_spans[row_index];

                uint32_t plan_index = UINT32_MAX;

                while (previous_cursor < previous_count &&
                       previous[previous_cursor].x0 < row->x0) {
                    ++previous_cursor;
                }

                if (previous_cursor < previous_count &&
                    previous[previous_cursor].x0 == row->x0 &&
                    previous[previous_cursor].x1 == row->x1 &&
                    previous[previous_cursor].first_window ==
                        row->first_window) {

                    uint32_t index =
                        previous[previous_cursor].plan_index;

                    if (index < g_compositor_render_plan_count &&
                        g_compositor_render_plan[index].rect.y1 ==
                            (int32_t)row_top) {

                        g_compositor_render_plan[index].rect.y1 =
                            row_bottom;
                        plan_index = index;
                    }
                }

                if (plan_index == UINT32_MAX) {
                    if (g_compositor_render_plan_count >=
                        WINDOW_RENDER_PLAN_MAX_SPANS) {
                        g_compositor_render_plan_count = 0U;
                        return false;
                    }

                    plan_index =
                        g_compositor_render_plan_count++;

                    g_compositor_render_plan[plan_index] =
                        (compositor_render_span_t){
                            .rect = {
                                .x0 = row->x0,
                                .y0 = row_top,
                                .x1 = row->x1,
                                .y1 = row_bottom,
                            },
                            .first_window = row->first_window,
                        };
                }

                if (current_count >= WINDOW_DAMAGE_MAX_TILES_X) {
                    g_compositor_render_plan_count = 0U;
                    return false;
                }

                current[current_count++] =
                    (compositor_render_active_span_t){
                        .x0 = row->x0,
                        .x1 = row->x1,
                        .first_window = row->first_window,
                        .plan_index = plan_index,
                    };
            }

            previous_count = current_count;

            for (uint32_t index = 0U;
                 index < current_count;
                 ++index) {
                previous[index] = current[index];
            }
        }
    }

    if (g_compositor_render_plan_count == 0U) {
        return false;
    }

    g_compositor_render_plan_valid = true;

    compositor_compile_render_plan_commands(snapshot);
    return true;
}
