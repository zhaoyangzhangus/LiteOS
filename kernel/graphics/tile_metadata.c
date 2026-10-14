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
    if (view != 0 && (view->flags & OS_WINDOW_PREMULTIPLIED_ALPHA) != 0U)
        return false;
    if (!tile_view_bounds(view, left, top, right, bottom)) return false;

    /* Client-owned surfaces are XRGB and cover their complete frame. */
    if (window_client_decorations(view->flags)) {
        if (!view->maximized) {
            if (*right - *left <= 16 || *bottom - *top <= 16) return false;
            *left += 8;
            *right -= 8;
            *top += 8;
            *bottom -= 8;
        }
        return true;
    }

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
    if (snapshot == 0 || metadata == 0 || out_columns == 0 ||
        out_rows == 0 || snapshot->window_count > WINDOW_SERVER_MAX_WINDOWS) {
        return false;
    }
    uint32_t columns = tile_count_for_extent(display_width, WINDOW_DAMAGE_MAX_TILES_X);
    uint32_t rows = tile_count_for_extent(display_height, WINDOW_DAMAGE_MAX_TILES_Y);
    if (columns == 0U || rows == 0U) return false;

    for (uint32_t y = 0U; y < rows; ++y) {
        for (uint32_t x = 0U; x < columns; ++x)
            metadata[y * WINDOW_DAMAGE_MAX_TILES_X + x] = (compositor_tile_meta_t){0};
    }

    /* Geometry is constant within a snapshot. Compute it once per window,
     * then visit only intersecting tiles, keeping masks in compacted order. */
    for (uint32_t window_index = 0U; window_index < snapshot->window_count; ++window_index) {
        const compositor_window_view_t *view = &snapshot->windows[window_index];
        int64_t left, top, right, bottom;
        int64_t opaque_left, opaque_top, opaque_right, opaque_bottom;
        if ((view->flags & OS_WINDOW_VISIBLE) == 0U ||
            !tile_view_bounds(view, &left, &top, &right, &bottom) ||
            !tile_intersects(left, top, right, bottom,
                             0, 0, display_width, display_height)) {
            continue;
        }
        uint32_t first_x = left > 0 ? (uint32_t)(left / WINDOW_DAMAGE_TILE_SIZE) : 0U;
        uint32_t first_y = top > 0 ? (uint32_t)(top / WINDOW_DAMAGE_TILE_SIZE) : 0U;
        int64_t clipped_right = right < (int64_t)display_width ? right : display_width;
        int64_t clipped_bottom = bottom < (int64_t)display_height ? bottom : display_height;
        uint32_t last_x = (uint32_t)((clipped_right - 1) / WINDOW_DAMAGE_TILE_SIZE);
        uint32_t last_y = (uint32_t)((clipped_bottom - 1) / WINDOW_DAMAGE_TILE_SIZE);
        if (last_x >= columns) last_x = columns - 1U;
        if (last_y >= rows) last_y = rows - 1U;
        bool opaque = tile_view_opaque_bounds(view, &opaque_left, &opaque_top,
                                             &opaque_right, &opaque_bottom);
        uint64_t bit = 1ULL << window_index;
        for (uint32_t y = first_y; y <= last_y; ++y) {
            for (uint32_t x = first_x; x <= last_x; ++x) {
                uint32_t index = y * WINDOW_DAMAGE_MAX_TILES_X + x;
                int64_t tile_left = (int64_t)x * WINDOW_DAMAGE_TILE_SIZE;
                int64_t tile_top = (int64_t)y * WINDOW_DAMAGE_TILE_SIZE;
                int64_t tile_right = tile_left + WINDOW_DAMAGE_TILE_SIZE;
                int64_t tile_bottom = tile_top + WINDOW_DAMAGE_TILE_SIZE;
                if (tile_right > (int64_t)display_width) tile_right = display_width;
                if (tile_bottom > (int64_t)display_height) tile_bottom = display_height;
                metadata[index].touch_mask |= bit;
                if (tile_is_covered(left, top, right, bottom, tile_left, tile_top,
                                    tile_right, tile_bottom))
                    metadata[index].full_mask |= bit;
                if (opaque && tile_is_covered(opaque_left, opaque_top, opaque_right,
                                              opaque_bottom, tile_left, tile_top,
                                              tile_right, tile_bottom))
                    metadata[index].opaque_mask |= bit;
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
        metadata[0].opaque_mask != 0U) {
        return false;
    }

    /* A restored surface touches the corner tile but does not fully occlude
     * it. The same maximized window is square and genuinely covers the tile. */
    snapshot.windows[0].maximized = true;
    if (!tile_metadata_build_into(&snapshot, 64U, 64U, metadata, &columns, &rows) ||
        metadata[0].opaque_mask != 0x1U) return false;
    snapshot.windows[0].maximized = false;

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
        metadata[0].opaque_mask != 0U ||
        metadata[1].touch_mask != 0x1U ||
        metadata[1].full_mask != 0x1U ||
        metadata[1].opaque_mask != 0U) {
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
           metadata[0].opaque_mask == 0U &&
           metadata[1].touch_mask == 0x1U &&
           metadata[1].full_mask == 0x1U &&
           metadata[1].opaque_mask == 0U;
}
