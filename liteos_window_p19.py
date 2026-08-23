#!/usr/bin/env python3
from pathlib import Path

SRC = Path("kernel/graphics/window_server.c")
s = SRC.read_text(encoding="utf-8")

if "g_compositor_occlusion_floor" not in s:
    raise SystemExit(
        "ERROR: P19 expects P18 to be applied first. "
        "Run liteos_window_p18.py before this script."
    )

if "g_compositor_render_plan" in s:
    print("P19 render-plan optimization already installed")
    raise SystemExit(0)

backup = SRC.with_suffix(".c.before-p19")
if not backup.exists():
    backup.write_text(s, encoding="utf-8")


def replace_once(text, old, new, name):
    if old not in text:
        raise SystemExit(f"ERROR: {name}: anchor not found")
    return text.replace(old, new, 1)


# ============================================================
# P19.1 - render-plan policy
# ============================================================

s = replace_once(
    s,
    '''#define WINDOW_OCCLUSION_CACHE_LARGE_PIXELS (1024U * 1024U)

#define WINDOW_SERVER_WORKER_STACK_SIZE (64U * 1024U)
''',
    '''#define WINDOW_OCCLUSION_CACHE_LARGE_PIXELS (1024U * 1024U)

/*
 * P19 per-frame render plan.
 *
 * 4096 entries cap static memory while covering normal sparse/tiled damage.
 * If a pathological frame exceeds the cap, the compositor simply falls back
 * to the P18 damage loop; correctness never depends on the plan.
 */
#define WINDOW_RENDER_PLAN_MAX_SPANS 4096U

#define WINDOW_SERVER_WORKER_STACK_SIZE (64U * 1024U)
''',
    "P19 policy constant",
)


# ============================================================
# P19.2 - render-plan storage
# ============================================================

s = replace_once(
    s,
    '''static bool g_compositor_occlusion_floor_valid;

/* True only for the scene frame switched through hidden StdVGA VRAM. */
''',
    '''static bool g_compositor_occlusion_floor_valid;

typedef struct compositor_render_span {
    window_damage_rect_t rect;

    /*
     * UINT32_MAX:
     *     desktop/below-window content is still required.
     *
     * otherwise:
     *     all pixels in rect are guaranteed to be overwritten by some
     *     window at or above first_window.
     */
    uint32_t first_window;
} compositor_render_span_t;

static compositor_render_span_t
    g_compositor_render_plan[WINDOW_RENDER_PLAN_MAX_SPANS];

static uint32_t g_compositor_render_plan_count;
static bool g_compositor_render_plan_valid;

/* True only for the scene frame switched through hidden StdVGA VRAM. */
''',
    "P19 plan storage",
)


# ============================================================
# P19.3 - make compositor_region_locked consume an optional plan
# ============================================================

s = replace_once(
    s,
    '''static void compositor_region_locked(void) {
''',
    '''static void compositor_region_locked(
    uint32_t planned_first_window,
    bool planned) {
''',
    "region signature",
)


old_floor = '''    /*
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
'''

new_floor = '''    uint32_t first_window;

    if (planned) {
        /*
         * P19 already resolved occlusion during snapshot planning.
         * No Z scan or tile-floor query remains in the render hot path.
         */
        first_window =
            planned_first_window;

        if (first_window == UINT32_MAX) {
            desktop_draw_wallpaper_locked();
            first_window = 0U;
        }
    } else {
        /*
         * P18 fallback for simple/pathological frames where no render plan
         * was built.
         */
        first_window =
            compositor_occlusion_floor_for_damage();

        if (first_window == UINT32_MAX) {
            first_window =
                compositor_topmost_damage_cover();
        }

        if (first_window == UINT32_MAX) {
            desktop_draw_wallpaper_locked();
            first_window = 0U;
        }
    }
'''

s = replace_once(
    s,
    old_floor,
    new_floor,
    "region first-window logic",
)


# ============================================================
# P19.4 - plan builder after immutable-view opaque helpers
# ============================================================

marker = '''/*
 * P16 capture phase.
 *
 * Called with window_lock held. Expensive planning is forbidden here.
 */
static bool compositor_snapshot_begin_locked(void) {
'''

if marker not in s:
    raise SystemExit("ERROR: P16 capture marker not found")

helpers = r'''/*
 * Exact rectangle floor using immutable snapshot views.
 *
 * This is used only for partial edge tiles that P18 cannot prove at full-tile
 * granularity.  It preserves the old rectangle-level precision without
 * bringing mutable window state or window_lock back into rendering.
 */
static uint32_t compositor_render_plan_exact_floor(
    const compositor_snapshot_t *snapshot,
    const window_damage_rect_t *rect) {

    if (snapshot == 0 || rect == 0 ||
        rect->left >= rect->right ||
        rect->top >= rect->bottom) {
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


/*
 * Read the P18 full-tile floor directly.
 */
static uint32_t compositor_render_plan_tile_floor(
    const compositor_snapshot_t *snapshot,
    uint32_t tile_x,
    uint32_t tile_y) {

    uint32_t columns;
    uint32_t rows;
    uint32_t value;

    if (snapshot == 0 ||
        !g_compositor_occlusion_floor_valid) {
        return UINT32_MAX;
    }

    columns = window_damage_tile_columns_locked();
    rows = window_damage_tile_rows_locked();

    if (tile_x >= columns ||
        tile_y >= rows) {
        return UINT32_MAX;
    }

    value =
        g_compositor_occlusion_floor[
            tile_y * WINDOW_DAMAGE_MAX_TILES_X +
            tile_x];

    if (value ==
            WINDOW_OCCLUSION_NO_FLOOR ||
        value >= snapshot->window_count) {
        return UINT32_MAX;
    }

    return value;
}


/*
 * Append one row span to the plan, or vertically merge it into the matching
 * span produced for the immediately preceding tile row.
 */
typedef struct compositor_render_active_span {
    uint32_t left;
    uint32_t right;
    uint32_t first_window;
    uint32_t plan_index;
} compositor_render_active_span_t;


typedef struct compositor_render_row_span {
    uint32_t left;
    uint32_t right;
    uint32_t first_window;
} compositor_render_row_span_t;


/*
 * Build one immutable render plan from snapshot damage + P18 tile floors.
 *
 * For a covered tile run:
 *     use the cached Z directly.
 *
 * For a NO_FLOOR tile:
 *     clip to the real damage rectangle and run one exact rectangle-cover test.
 *
 * Adjacent same-Z spans are merged horizontally first, then identical spans
 * across adjacent tile rows are merged vertically.
 *
 * The result is typically far smaller than "one span per tile".
 */
static bool compositor_build_render_plan(
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

    /*
     * P19 intentionally piggybacks on P18.  If P18 judged the frame too small
     * for a tile cache, keep the old simple path rather than constructing a
     * plan that cannot amortize its setup cost.
     */
    if (snapshot == 0 ||
        !g_compositor_occlusion_floor_valid ||
        snapshot->damage_count == 0U) {
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

        const window_damage_rect_t *damage =
            &snapshot->damage_rects[damage_index];

        uint32_t first_tile_x;
        uint32_t last_tile_x;
        uint32_t first_tile_y;
        uint32_t last_tile_y;

        uint32_t previous_count = 0U;

        if (damage->left >= damage->right ||
            damage->top >= damage->bottom) {
            continue;
        }

        first_tile_x =
            damage->left /
            WINDOW_DAMAGE_TILE_SIZE;

        last_tile_x =
            (damage->right - 1U) /
            WINDOW_DAMAGE_TILE_SIZE;

        first_tile_y =
            damage->top /
            WINDOW_DAMAGE_TILE_SIZE;

        last_tile_y =
            (damage->bottom - 1U) /
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

            uint32_t tile_x =
                first_tile_x;

            uint32_t row_top =
                tile_y * WINDOW_DAMAGE_TILE_SIZE;

            uint32_t row_bottom =
                row_top + WINDOW_DAMAGE_TILE_SIZE;

            if (row_top < damage->top)
                row_top = damage->top;

            if (row_bottom > damage->bottom)
                row_bottom = damage->bottom;

            while (tile_x <= last_tile_x) {
                uint32_t floor =
                    compositor_render_plan_tile_floor(
                        snapshot,
                        tile_x,
                        tile_y);

                uint32_t run_start =
                    tile_x;

                uint32_t run_end =
                    tile_x + 1U;

                /*
                 * A proven P18 floor may be grouped across adjacent tiles with
                 * the same floor.  A NO_FLOOR tile stays isolated so the exact
                 * fallback can recover precision for a small partial edge.
                 */
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
                    run_start *
                    WINDOW_DAMAGE_TILE_SIZE;

                uint32_t right =
                    run_end *
                    WINDOW_DAMAGE_TILE_SIZE;

                if (left < damage->left)
                    left = damage->left;

                if (right > damage->right)
                    right = damage->right;

                if (left < right &&
                    row_top < row_bottom) {

                    if (floor == UINT32_MAX) {
                        window_damage_rect_t exact_rect = {
                            .left = left,
                            .top = row_top,
                            .right = right,
                            .bottom = row_bottom,
                        };

                        floor =
                            compositor_render_plan_exact_floor(
                                snapshot,
                                &exact_rect);
                    }

                    /*
                     * Horizontal merge after the exact fallback: two adjacent
                     * partial/full tile runs that resolved to the same Z become
                     * one row span.
                     */
                    if (row_count != 0U &&
                        row_spans[row_count - 1U].right == left &&
                        row_spans[row_count - 1U].first_window == floor) {

                        row_spans[row_count - 1U].right =
                            right;

                    } else {
                        if (row_count >=
                            WINDOW_DAMAGE_MAX_TILES_X) {
                            g_compositor_render_plan_count = 0U;
                            return false;
                        }

                        row_spans[row_count++] =
                            (compositor_render_row_span_t){
                                .left = left,
                                .right = right,
                                .first_window = floor,
                            };
                    }
                }

                tile_x = run_end;
            }

            /*
             * row_spans[] is sorted by left.  previous[] is also sorted, so a
             * single forward cursor is sufficient for vertical merging.
             */
            for (uint32_t row_index = 0U;
                 row_index < row_count;
                 ++row_index) {

                compositor_render_row_span_t *row =
                    &row_spans[row_index];

                uint32_t plan_index =
                    UINT32_MAX;

                while (previous_cursor < previous_count &&
                       previous[previous_cursor].left <
                           row->left) {
                    ++previous_cursor;
                }

                if (previous_cursor < previous_count &&
                    previous[previous_cursor].left ==
                        row->left &&
                    previous[previous_cursor].right ==
                        row->right &&
                    previous[previous_cursor].first_window ==
                        row->first_window) {

                    uint32_t index =
                        previous[previous_cursor].plan_index;

                    if (index <
                            g_compositor_render_plan_count &&
                        g_compositor_render_plan[index]
                                .rect.bottom ==
                            row_top) {

                        g_compositor_render_plan[index]
                            .rect.bottom =
                                row_bottom;

                        plan_index =
                            index;
                    }
                }

                if (plan_index == UINT32_MAX) {
                    if (g_compositor_render_plan_count >=
                        WINDOW_RENDER_PLAN_MAX_SPANS) {

                        /*
                         * Pathological checkerboard/overlap pattern.  Drop the
                         * partial plan and fall back to P18.
                         */
                        g_compositor_render_plan_count = 0U;
                        return false;
                    }

                    plan_index =
                        g_compositor_render_plan_count++;

                    g_compositor_render_plan[plan_index] =
                        (compositor_render_span_t){
                            .rect = {
                                .left = row->left,
                                .top = row_top,
                                .right = row->right,
                                .bottom = row_bottom,
                            },
                            .first_window =
                                row->first_window,
                        };
                }

                if (current_count >=
                    WINDOW_DAMAGE_MAX_TILES_X) {

                    g_compositor_render_plan_count = 0U;
                    return false;
                }

                current[current_count++] =
                    (compositor_render_active_span_t){
                        .left = row->left,
                        .right = row->right,
                        .first_window =
                            row->first_window,
                        .plan_index =
                            plan_index,
                    };
            }

            previous_count =
                current_count;

            for (uint32_t index = 0U;
                 index < current_count;
                 ++index) {
                previous[index] =
                    current[index];
            }
        }
    }

    if (g_compositor_render_plan_count == 0U) {
        return false;
    }

    g_compositor_render_plan_valid =
        true;

    return true;
}


'''

insert_at = s.index(marker)
s = s[:insert_at] + helpers + s[insert_at:]


# ============================================================
# P19.5 - build plan after P18 cache
# ============================================================

s = replace_once(
    s,
    '''    compositor_build_occlusion_floor_cache(
        snapshot);

    snapshot->damage_full_captured =
''',
    '''    compositor_build_occlusion_floor_cache(
        snapshot);

    /*
     * P19 consumes the P18 floor cache and materializes the final spatial
     * render program.  Failure merely selects the existing P18 path.
     */
    (void)compositor_build_render_plan(
        snapshot);

    snapshot->damage_full_captured =
''',
    "render plan build call",
)


# ============================================================
# P19.6 - render the plan directly
# ============================================================

old_render_loop = '''    for (uint32_t index = 0U;
         index < g_compositor_snapshot.damage_count;
         ++index) {

        const window_damage_rect_t *rect =
            &g_compositor_snapshot.damage_rects[index];

        g_compositor_snapshot.damage_left =
            rect->left;

        g_compositor_snapshot.damage_top =
            rect->top;

        g_compositor_snapshot.damage_right =
            rect->right;

        g_compositor_snapshot.damage_bottom =
            rect->bottom;

        compositor_region_locked();
    }
'''

new_render_loop = '''    if (g_compositor_render_plan_valid) {
        /*
         * P19 hot path: occlusion/planning is already complete.
         */
        for (uint32_t index = 0U;
             index < g_compositor_render_plan_count;
             ++index) {

            const compositor_render_span_t *span =
                &g_compositor_render_plan[index];

            g_compositor_snapshot.damage_left =
                span->rect.left;

            g_compositor_snapshot.damage_top =
                span->rect.top;

            g_compositor_snapshot.damage_right =
                span->rect.right;

            g_compositor_snapshot.damage_bottom =
                span->rect.bottom;

            compositor_region_locked(
                span->first_window,
                true);
        }
    } else {
        /*
         * Simple frame or pathological plan overflow: retain P18 exactly.
         */
        for (uint32_t index = 0U;
             index < g_compositor_snapshot.damage_count;
             ++index) {

            const window_damage_rect_t *rect =
                &g_compositor_snapshot.damage_rects[index];

            g_compositor_snapshot.damage_left =
                rect->left;

            g_compositor_snapshot.damage_top =
                rect->top;

            g_compositor_snapshot.damage_right =
                rect->right;

            g_compositor_snapshot.damage_bottom =
                rect->bottom;

            compositor_region_locked(
                UINT32_MAX,
                false);
        }
    }
'''

s = replace_once(
    s,
    old_render_loop,
    new_render_loop,
    "render loop",
)


# ============================================================
# P19.7 - clear per-frame plan metadata at finish
# ============================================================

s = replace_once(
    s,
    '''    g_compositor_snapshot.window_count = 0U;
    g_compositor_snapshot.damage_count = 0U;

    /*
''',
    '''    g_compositor_snapshot.window_count = 0U;
    g_compositor_snapshot.damage_count = 0U;

    g_compositor_render_plan_count = 0U;
    g_compositor_render_plan_valid = false;

    /*
''',
    "finish plan reset",
)


SRC.write_text(s, encoding="utf-8")

print("OK: P19 per-frame tile-span render plan installed")
print(f"backup: {backup}")
