#!/usr/bin/env python3
from pathlib import Path

SRC = Path("kernel/graphics/window_server.c")
s = SRC.read_text(encoding="utf-8")

if "compositor_snapshot_plan" not in s or "damage_tiles_captured" not in s:
    raise SystemExit(
        "ERROR: P18 expects P16 to be applied first. "
        "Run liteos_window_p16_p17.py before this script."
    )

if "g_compositor_occlusion_floor" in s:
    print("P18 tile occlusion cache already installed")
    raise SystemExit(0)

backup = SRC.with_suffix(".c.before-p18")
if not backup.exists():
    backup.write_text(s, encoding="utf-8")


def replace_once(text, old, new, name):
    if old not in text:
        raise SystemExit(f"ERROR: {name}: anchor not found")
    return text.replace(old, new, 1)


# ============================================================
# P18.1 - policy constants
# ============================================================

s = replace_once(
    s,
    '''#define WINDOW_DAMAGE_MAX_SNAPSHOT_RECTS 4096U
#define WINDOW_SERVER_WORKER_STACK_SIZE (64U * 1024U)
''',
    '''#define WINDOW_DAMAGE_MAX_SNAPSHOT_RECTS 4096U

/*
 * P18 tile-level occlusion.
 *
 * One byte stores the highest snapshot Z index whose guaranteed opaque
 * interior completely covers a tile.  0xFF means desktop/below-Z content may
 * still be required.
 *
 * Building the cache has a cost, so simple frames retain the old precise
 * per-damage reverse scan.  Complex/large frames build once and then query in
 * O(number_of_damage_tiles).
 */
#define WINDOW_OCCLUSION_NO_FLOOR 0xFFU
#define WINDOW_OCCLUSION_CACHE_MIN_SCORE 32U
#define WINDOW_OCCLUSION_CACHE_LARGE_PIXELS (1024U * 1024U)

#define WINDOW_SERVER_WORKER_STACK_SIZE (64U * 1024U)
''',
    "P18 constants",
)


# ============================================================
# P18.2 - cache storage
# ============================================================

s = replace_once(
    s,
    '''static compositor_snapshot_t g_compositor_snapshot;

/* True only for the scene frame switched through hidden StdVGA VRAM. */
''',
    '''static compositor_snapshot_t g_compositor_snapshot;

/*
 * Snapshot-local tile floor cache.
 *
 * The compositor is single-owner (g_window_server.composing), so this does not
 * need atomics.  Keep it outside compositor_snapshot_t so the already-large
 * immutable snapshot does not carry an extra 16 KiB through unrelated code.
 */
static uint8_t g_compositor_occlusion_floor[WINDOW_DAMAGE_TILE_COUNT]
    __attribute__((aligned(64)));

static bool g_compositor_occlusion_floor_valid;

/* True only for the scene frame switched through hidden StdVGA VRAM. */
''',
    "P18 cache storage",
)


# ============================================================
# P18.3 - cache builder/query, inserted before compositor_region_locked()
# ============================================================

marker = "static void compositor_region_locked(void) {"
if marker not in s:
    raise SystemExit("ERROR: compositor_region_locked marker not found")

helpers = r'''/*
 * Return the full-tile X/Y index range contained by one clipped opaque
 * interval.
 */
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

    /*
     * If the opaque interval reaches the display edge, include the final
     * partial screen tile as a complete visible tile.
     */
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


/*
 * Build the highest guaranteed opaque Z for each screen tile.
 *
 * Windows are traversed bottom -> top, therefore later stores naturally
 * replace lower covers with stronger covers.
 */
static void compositor_build_occlusion_floor_cache(
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

        const window_damage_rect_t *rect =
            &snapshot->damage_rects[index];

        if (rect->left >= rect->right ||
            rect->top >= rect->bottom) {
            continue;
        }

        damage_pixels +=
            (uint64_t)(rect->right - rect->left) *
            (uint64_t)(rect->bottom - rect->top);
    }

    /*
     * Small/simple frames retain the existing exact reverse scan.
     */
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


/*
 * Return the strongest safe starting Z for the CURRENT damage rectangle.
 *
 * Different tiles may be hidden by different opaque windows.  The minimum
 * per-tile floor is still safe for the whole rectangle because every tile is
 * guaranteed to be overwritten by some window at or above that Z.
 *
 * UINT32_MAX means the tile cache cannot prove desktop/lower-Z independence.
 */
static uint32_t compositor_occlusion_floor_for_damage(void) {
    uint32_t columns;
    uint32_t rows;

    uint32_t first_x;
    uint32_t last_x;
    uint32_t first_y;
    uint32_t last_y;

    uint32_t floor = UINT32_MAX;

    if (!g_compositor_occlusion_floor_valid ||
        g_compositor_snapshot.damage_left >=
            g_compositor_snapshot.damage_right ||
        g_compositor_snapshot.damage_top >=
            g_compositor_snapshot.damage_bottom) {
        return UINT32_MAX;
    }

    columns = window_damage_tile_columns_locked();
    rows = window_damage_tile_rows_locked();

    if (columns == 0U || rows == 0U) {
        return UINT32_MAX;
    }

    first_x =
        g_compositor_snapshot.damage_left /
        WINDOW_DAMAGE_TILE_SIZE;

    last_x =
        (g_compositor_snapshot.damage_right - 1U) /
        WINDOW_DAMAGE_TILE_SIZE;

    first_y =
        g_compositor_snapshot.damage_top /
        WINDOW_DAMAGE_TILE_SIZE;

    last_y =
        (g_compositor_snapshot.damage_bottom - 1U) /
        WINDOW_DAMAGE_TILE_SIZE;

    if (first_x >= columns ||
        first_y >= rows) {
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

            if (candidate ==
                WINDOW_OCCLUSION_NO_FLOOR) {
                return UINT32_MAX;
            }

            if (candidate >=
                g_compositor_snapshot.window_count) {
                return UINT32_MAX;
            }

            if (floor == UINT32_MAX ||
                candidate < floor) {
                floor = candidate;
            }
        }
    }

    return floor;
}


'''

insert_at = s.index(marker)
s = s[:insert_at] + helpers + s[insert_at:]


# ============================================================
# P18.4 - cache first, exact reverse scan as precision fallback
# ============================================================

s = replace_once(
    s,
    '''    uint32_t first_window =
        compositor_topmost_damage_cover();

    if (first_window == UINT32_MAX) {
        /*
         * No window completely hides this damage; rebuild the desktop and
         * compose from the bottom as usual.
         */
        desktop_draw_wallpaper_locked();
        first_window = 0U;
    }
''',
    '''    /*
     * P18 first tries collective tile-level occlusion. If a partial edge tile
     * cannot be proven opaque, retain the old exact per-rectangle reverse scan
     * as a precision fallback.
     */
    uint32_t first_window =
        compositor_occlusion_floor_for_damage();

    if (first_window == UINT32_MAX) {
        first_window =
            compositor_topmost_damage_cover();
    }

    if (first_window == UINT32_MAX) {
        desktop_draw_wallpaper_locked();
        first_window = 0U;
    }
''',
    "region tile-floor lookup",
)


# ============================================================
# P18.5 - build once per P16 immutable snapshot
# ============================================================

s = replace_once(
    s,
    '''    snapshot->window_count =
        write_index;

    snapshot->damage_full_captured =
        false;

    snapshot->damage_tiles_captured =
        false;
}
''',
    '''    snapshot->window_count =
        write_index;

    /*
     * Adaptive: the builder immediately returns for simple frames.
     */
    compositor_build_occlusion_floor_cache(
        snapshot);

    snapshot->damage_full_captured =
        false;

    snapshot->damage_tiles_captured =
        false;
}
''',
    "P18 build call",
)


SRC.write_text(s, encoding="utf-8")

print("OK: P18 adaptive tile-level occlusion cache installed")
print(f"backup: {backup}")
