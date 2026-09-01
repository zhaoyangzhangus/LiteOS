#include "internal.h"
#include <kernel/perf.h>
#include <kernel/telemetry.h>

/* REFACTOR_P7F_TILE_METADATA_OWNER: per-frame tile masks and self-test. */

_Static_assert(WINDOW_SERVER_MAX_WINDOWS <= 64U,
               "tile metadata masks require <= 64 windows");

compositor_tile_meta_t g_compositor_tile_meta[WINDOW_DAMAGE_TILE_COUNT]
    __attribute__((aligned(64)));

static uint32_t g_compositor_tile_columns;
static uint32_t g_compositor_tile_rows;
static bool g_compositor_tile_meta_valid;

static uint32_t tile_count_for_extent(uint32_t extent, uint32_t limit) {
    uint32_t count =
        (extent + WINDOW_DAMAGE_TILE_SIZE - 1U) /
        WINDOW_DAMAGE_TILE_SIZE;
    return count > limit ? limit : count;
}

static bool tile_view_bounds(const compositor_window_view_t *view,
                             int64_t *left, int64_t *top,
                             int64_t *right, int64_t *bottom) {
    if (view == 0 || left == 0 || top == 0 || right == 0 || bottom == 0) {
        return false;
    }

    *left = view->x;
    *top = view->y;
    *right =
        *left +
        (int64_t)window_outer_width(view->width, view->flags);
    *bottom =
        *top +
        (int64_t)window_outer_height(view->height, view->flags);

    return *left < *right && *top < *bottom;
}

static bool tile_intersects(int64_t left, int64_t top,
                            int64_t right, int64_t bottom,
                            int64_t tile_left, int64_t tile_top,
                            int64_t tile_right, int64_t tile_bottom) {
    return left < tile_right && right > tile_left &&
           top < tile_bottom && bottom > tile_top;
}

static bool tile_is_covered(int64_t left, int64_t top,
                            int64_t right, int64_t bottom,
                            int64_t tile_left, int64_t tile_top,
                            int64_t tile_right, int64_t tile_bottom) {
    return left <= tile_left && top <= tile_top &&
           right >= tile_right && bottom >= tile_bottom;
}

static bool tile_view_opaque_bounds(const compositor_window_view_t *view,
                                    int64_t *left, int64_t *top,
                                    int64_t *right, int64_t *bottom) {
    if (!tile_view_bounds(view, left, top, right, bottom)) return false;

    /* Client-owned surfaces are XRGB and cover their complete frame. */
    if (window_client_decorations(view->flags)) return true;

    if (*right - *left <= (int64_t)(WINDOW_CORNER_RADIUS * 2U) ||
        *bottom - *top <= (int64_t)(WINDOW_CORNER_RADIUS * 2U)) {
        return false;
    }

    *left += WINDOW_CORNER_RADIUS;
    *top += WINDOW_CORNER_RADIUS;
    *right -= WINDOW_CORNER_RADIUS;
    *bottom -= WINDOW_CORNER_RADIUS;
    return true;
}

static bool tile_metadata_build_into(
    const compositor_snapshot_t *snapshot,
    uint32_t display_width,
    uint32_t display_height,
    compositor_tile_meta_t *metadata,
    uint32_t *out_columns,
    uint32_t *out_rows) {
    uint32_t columns;
    uint32_t rows;

    if (snapshot == 0 || metadata == 0 || out_columns == 0 ||
        out_rows == 0 || snapshot->window_count > WINDOW_SERVER_MAX_WINDOWS) {
        return false;
    }

    columns =
        tile_count_for_extent(
            display_width,
            WINDOW_DAMAGE_MAX_TILES_X);

    rows =
        tile_count_for_extent(
            display_height,
            WINDOW_DAMAGE_MAX_TILES_Y);

    if (columns == 0U || rows == 0U) return false;

    for (uint32_t tile_y = 0U; tile_y < rows; ++tile_y) {
        for (uint32_t tile_x = 0U; tile_x < columns; ++tile_x) {
            uint32_t index =
                tile_y * WINDOW_DAMAGE_MAX_TILES_X + tile_x;

            metadata[index] =
                (compositor_tile_meta_t){0};

            int64_t tile_left =
                (int64_t)tile_x * WINDOW_DAMAGE_TILE_SIZE;
            int64_t tile_top =
                (int64_t)tile_y * WINDOW_DAMAGE_TILE_SIZE;
            int64_t tile_right =
                tile_left + WINDOW_DAMAGE_TILE_SIZE;
            int64_t tile_bottom =
                tile_top + WINDOW_DAMAGE_TILE_SIZE;

            if (tile_right > (int64_t)display_width) {
                tile_right = display_width;
            }

            if (tile_bottom > (int64_t)display_height) {
                tile_bottom = display_height;
            }

            for (uint32_t window_index = 0U;
                 window_index < snapshot->window_count;
                 ++window_index) {
                const compositor_window_view_t *view =
                    &snapshot->windows[window_index];
                int64_t left;
                int64_t top;
                int64_t right;
                int64_t bottom;
                uint64_t bit = 1ULL << window_index;

                if ((view->flags & OS_WINDOW_VISIBLE) == 0U ||
                    !tile_view_bounds(
                        view,
                        &left,
                        &top,
                        &right,
                        &bottom) ||
                    !tile_intersects(
                        left,
                        top,
                        right,
                        bottom,
                        tile_left,
                        tile_top,
                        tile_right,
                        tile_bottom)) {
                    continue;
                }

                metadata[index].touch_mask |= bit;

                if (tile_is_covered(
                        left,
                        top,
                        right,
                        bottom,
                        tile_left,
                        tile_top,
                        tile_right,
                        tile_bottom)) {
                    metadata[index].full_mask |= bit;
                }

                if (tile_view_opaque_bounds(
                        view,
                        &left,
                        &top,
                        &right,
                        &bottom) &&
                    tile_is_covered(
                        left,
                        top,
                        right,
                        bottom,
                        tile_left,
                        tile_top,
                        tile_right,
                        tile_bottom)) {
                    metadata[index].opaque_mask |= bit;
                }
            }
        }
    }

    *out_columns = columns;
    *out_rows = rows;
    return true;
}

bool compositor_tile_metadata_build(
    const compositor_snapshot_t *snapshot) {
    uint32_t columns = 0U;
    uint32_t rows = 0U;

    g_compositor_tile_meta_valid = false;

    if (!tile_metadata_build_into(
            snapshot,
            g_window_server.display_width,
            g_window_server.display_height,
            g_compositor_tile_meta,
            &columns,
            &rows)) {
        g_compositor_tile_columns = 0U;
        g_compositor_tile_rows = 0U;
        return false;
    }

    g_compositor_tile_columns = columns;
    g_compositor_tile_rows = rows;
    g_compositor_tile_meta_valid = true;
    return true;
}

bool compositor_tile_metadata_valid(void) {
    return g_compositor_tile_meta_valid;
}

const compositor_tile_meta_t *compositor_tile_metadata_at(
    uint32_t tile_x,
    uint32_t tile_y) {
    if (!g_compositor_tile_meta_valid ||
        tile_x >= g_compositor_tile_columns ||
        tile_y >= g_compositor_tile_rows) {
        return 0;
    }

    return &g_compositor_tile_meta[
        tile_y * WINDOW_DAMAGE_MAX_TILES_X + tile_x];
}

bool compositor_tile_self_test(void) {
    compositor_snapshot_t snapshot;
    compositor_tile_meta_t metadata[2];
    uint32_t columns = 0U;
    uint32_t rows = 0U;
    uint64_t benchmark_start;

    snapshot.window_count = 1U;
    snapshot.windows[0] = (compositor_window_view_t){
        .x = 0,
        .y = 0,
        .width = 128U,
        .height = 64U,
        .flags = OS_WINDOW_VISIBLE | OS_WINDOW_CLIENT_DECORATIONS,
    };

    if (!tile_metadata_build_into(
            &snapshot,
            64U,
            64U,
            metadata,
            &columns,
            &rows) ||
        columns != 1U ||
        rows != 1U ||
        metadata[0].touch_mask != 0x1U ||
        metadata[0].full_mask != 0x1U ||
        metadata[0].opaque_mask != 0x1U) {
        return false;
    }

    snapshot.window_count = 2U;
    snapshot.windows[1] = (compositor_window_view_t){
        .x = 32,
        .y = 0,
        .width = 32U,
        .height = 64U,
        .flags = OS_WINDOW_VISIBLE | OS_WINDOW_CLIENT_DECORATIONS,
    };

    if (!tile_metadata_build_into(
            &snapshot,
            128U,
            64U,
            metadata,
            &columns,
            &rows) ||
        columns != 2U ||
        rows != 1U) {
        return false;
    }

    if (metadata[0].touch_mask != 0x3U ||
        metadata[0].full_mask != 0x1U ||
        metadata[0].opaque_mask != 0x1U ||
        metadata[1].touch_mask != 0x1U ||
        metadata[1].full_mask != 0x1U ||
        metadata[1].opaque_mask != 0x1U) {
        return false;
    }

    for (uint32_t index = 2U; index < 8U; ++index) {
        snapshot.windows[index] = (compositor_window_view_t){
            .x = (int32_t)(index * 4U),
            .y = 0,
            .width = 32U,
            .height = 64U,
            .flags = OS_WINDOW_VISIBLE | OS_WINDOW_CLIENT_DECORATIONS,
        };
    }

    snapshot.window_count = 4U;
    benchmark_start = telemetry_timestamp();
    if (!tile_metadata_build_into(
            &snapshot,
            128U,
            64U,
            metadata,
            &columns,
            &rows) ||
        columns != 2U ||
        rows != 1U) {
        return false;
    }
    kernel_perf_emit_scope("graphics.overlap_4", benchmark_start);

    snapshot.window_count = 8U;
    benchmark_start = telemetry_timestamp();
    if (!tile_metadata_build_into(
            &snapshot,
            128U,
            64U,
            metadata,
            &columns,
            &rows) ||
        columns != 2U ||
        rows != 1U) {
        return false;
    }
    kernel_perf_emit_scope("graphics.overlap_8", benchmark_start);

    return metadata[0].touch_mask == 0xFFU &&
           metadata[0].full_mask == 0x1U &&
           metadata[0].opaque_mask == 0x1U &&
           metadata[1].touch_mask == 0x1U &&
           metadata[1].full_mask == 0x1U &&
           metadata[1].opaque_mask == 0x1U;
}
