#include "internal.h"

/* REFACTOR_P7G_OCCLUSION_CACHE_OWNER: tile-level opaque floor cache. */

#define WINDOW_OCCLUSION_CACHE_MIN_SCORE 32U
#define WINDOW_OCCLUSION_CACHE_LARGE_PIXELS (1024U * 1024U)

/* P18 compatibility floor cache now has a tile-owner lifetime. */
uint8_t g_compositor_occlusion_floor[WINDOW_DAMAGE_TILE_COUNT]
    __attribute__((aligned(64)));
bool g_compositor_occlusion_floor_valid;

static void compositor_occlusion_full_tile_range(
    int64_t start,
    int64_t end,
    uint32_t extent,
    uint32_t tile_count,
    uint32_t *first,
    uint32_t *last_exclusive) {

    uint32_t begin;
    uint32_t finish;

    if (first == 0 || last_exclusive == 0 ||
        extent == 0U || tile_count == 0U ||
        start >= end || end <= 0 ||
        start >= (int64_t)extent) {

        if (first != 0) *first = 0U;
        if (last_exclusive != 0) *last_exclusive = 0U;
        return;
    }

    if (start < 0) start = 0;
    if (end > (int64_t)extent) end = extent;

    begin =
        (uint32_t)(
            ((uint64_t)start +
             WINDOW_DAMAGE_TILE_SIZE - 1U) /
            WINDOW_DAMAGE_TILE_SIZE);

    /* Include the final partial screen tile at the display edge. */
    if (end == (int64_t)extent) {
        finish = tile_count;
    } else {
        finish =
            (uint32_t)(
                (uint64_t)end /
                WINDOW_DAMAGE_TILE_SIZE);
    }

    if (begin > tile_count) begin = tile_count;
    if (finish > tile_count) finish = tile_count;
    if (finish < begin) finish = begin;

    *first = begin;
    *last_exclusive = finish;
}

void compositor_build_occlusion_floor_cache(
    const compositor_snapshot_t *snapshot) {

    uint32_t columns;
    uint32_t rows;
    uint64_t damage_pixels = 0U;
    uint64_t score;

    g_compositor_occlusion_floor_valid = false;

    if (snapshot == 0 ||
        snapshot->window_count < 2U ||
        snapshot->damage_count == 0U) {
        return;
    }

    columns = window_damage_tile_columns_locked();
    rows = window_damage_tile_rows_locked();

    if (columns == 0U || rows == 0U) {
        return;
    }

    score =
        (uint64_t)snapshot->window_count *
        snapshot->damage_count;

    for (uint32_t index = 0U;
         index < snapshot->damage_count;
         ++index) {

        const Rect *rect =
            &snapshot->damage_rects[index];

        if (rect->x0 >= rect->x1 ||
            rect->y0 >= rect->y1) {
            continue;
        }

        damage_pixels +=
            (uint64_t)(rect->x1 - rect->x0) *
            (uint64_t)(rect->y1 - rect->y0);
    }

    /* Small/simple frames retain the existing exact reverse scan. */
    if (score < WINDOW_OCCLUSION_CACHE_MIN_SCORE &&
        !(damage_pixels >= WINDOW_OCCLUSION_CACHE_LARGE_PIXELS &&
          snapshot->window_count >= 4U)) {
        return;
    }

    for (uint32_t tile_y = 0U;
         tile_y < rows;
         ++tile_y) {

        uint32_t base =
            tile_y * WINDOW_DAMAGE_MAX_TILES_X;

        for (uint32_t tile_x = 0U;
             tile_x < columns;
             ++tile_x) {

            g_compositor_occlusion_floor[
                base + tile_x] =
                WINDOW_OCCLUSION_NO_FLOOR;
        }
    }

    for (uint32_t z = 0U;
         z < snapshot->window_count;
         ++z) {

        const compositor_window_view_t *window =
            &snapshot->windows[z];

        uint64_t outer_width;
        uint64_t outer_height;

        int64_t opaque_left;
        int64_t opaque_top;
        int64_t opaque_right;
        int64_t opaque_bottom;

        uint32_t first_x;
        uint32_t last_x;
        uint32_t first_y;
        uint32_t last_y;

        if ((window->flags & OS_WINDOW_VISIBLE) == 0U ||
            z >= WINDOW_OCCLUSION_NO_FLOOR) {
            continue;
        }

        outer_width =
            (uint64_t)window_outer_width(
                window->width,
                window->flags);

        outer_height =
            (uint64_t)window_outer_height(
                window->height,
                window->flags);

        if (outer_width <= WINDOW_CORNER_RADIUS * 2U ||
            outer_height <= WINDOW_CORNER_RADIUS * 2U) {
            continue;
        }

        opaque_left =
            (int64_t)window->x +
            WINDOW_CORNER_RADIUS;

        opaque_top =
            (int64_t)window->y +
            WINDOW_CORNER_RADIUS;

        opaque_right =
            (int64_t)window->x +
            (int64_t)outer_width -
            WINDOW_CORNER_RADIUS;

        opaque_bottom =
            (int64_t)window->y +
            (int64_t)outer_height -
            WINDOW_CORNER_RADIUS;

        compositor_occlusion_full_tile_range(
            opaque_left,
            opaque_right,
            g_window_server.display_width,
            columns,
            &first_x,
            &last_x);

        compositor_occlusion_full_tile_range(
            opaque_top,
            opaque_bottom,
            g_window_server.display_height,
            rows,
            &first_y,
            &last_y);

        if (first_x >= last_x ||
            first_y >= last_y) {
            continue;
        }

        for (uint32_t tile_y = first_y;
             tile_y < last_y;
             ++tile_y) {

            uint32_t base =
                tile_y * WINDOW_DAMAGE_MAX_TILES_X;

            for (uint32_t tile_x = first_x;
                 tile_x < last_x;
                 ++tile_x) {

                g_compositor_occlusion_floor[
                    base + tile_x] =
                    (uint8_t)z;
            }
        }
    }

    g_compositor_occlusion_floor_valid = true;
}

uint32_t compositor_occlusion_floor_for_damage(void) {
    uint32_t columns;
    uint32_t rows;

    uint32_t first_x;
    uint32_t last_x;
    uint32_t first_y;
    uint32_t last_y;

    uint32_t floor = UINT32_MAX;

    if (!g_compositor_occlusion_floor_valid ||
        g_compositor_snapshot.damage_bounds.x0 >=
            g_compositor_snapshot.damage_bounds.x1 ||
        g_compositor_snapshot.damage_bounds.y0 >=
            g_compositor_snapshot.damage_bounds.y1) {
        return UINT32_MAX;
    }

    columns = window_damage_tile_columns_locked();
    rows = window_damage_tile_rows_locked();

    if (columns == 0U || rows == 0U) {
        return UINT32_MAX;
    }

    first_x =
        g_compositor_snapshot.damage_bounds.x0 /
        WINDOW_DAMAGE_TILE_SIZE;

    last_x =
        (g_compositor_snapshot.damage_bounds.x1 - 1U) /
        WINDOW_DAMAGE_TILE_SIZE;

    first_y =
        g_compositor_snapshot.damage_bounds.y0 /
        WINDOW_DAMAGE_TILE_SIZE;

    last_y =
        (g_compositor_snapshot.damage_bounds.y1 - 1U) /
        WINDOW_DAMAGE_TILE_SIZE;

    if (first_x >= columns || first_y >= rows) {
        return UINT32_MAX;
    }

    if (last_x >= columns) last_x = columns - 1U;
    if (last_y >= rows) last_y = rows - 1U;

    for (uint32_t tile_y = first_y;
         tile_y <= last_y;
         ++tile_y) {

        uint32_t base =
            tile_y * WINDOW_DAMAGE_MAX_TILES_X;

        for (uint32_t tile_x = first_x;
             tile_x <= last_x;
             ++tile_x) {

            uint32_t candidate =
                g_compositor_occlusion_floor[
                    base + tile_x];

            if (candidate == WINDOW_OCCLUSION_NO_FLOOR ||
                candidate >= g_compositor_snapshot.window_count) {
                return UINT32_MAX;
            }

            if (floor == UINT32_MAX || candidate < floor) {
                floor = candidate;
            }
        }
    }

    return floor;
}
