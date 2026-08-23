#!/usr/bin/env python3
from pathlib import Path

SRC = Path("kernel/graphics/window_server.c")
HDR = Path("include/kernel/window_server.h")

s = SRC.read_text(encoding="utf-8")
h = HDR.read_text(encoding="utf-8")

src_backup = SRC.with_suffix(".c.before-p16-p17")
hdr_backup = HDR.with_suffix(".h.before-p16-p17")

if not src_backup.exists():
    src_backup.write_text(s, encoding="utf-8")
if not hdr_backup.exists():
    hdr_backup.write_text(h, encoding="utf-8")


def replace_once(text, old, new, name):
    if old not in text:
        raise SystemExit(f"ERROR: {name}: anchor not found")
    return text.replace(old, new, 1)


# ============================================================
# P16.1 - snapshot carries captured producer tile state
# ============================================================

if "damage_tiles_captured" not in s:
    s = replace_once(
        s,
        '''    uint32_t damage_count;
    window_damage_rect_t damage_rects[WINDOW_DAMAGE_MAX_SNAPSHOT_RECTS];

    /*
     * Current region being rendered.  Only the single active compositor
''',
        '''    uint32_t damage_count;
    window_damage_rect_t damage_rects[WINDOW_DAMAGE_MAX_SNAPSHOT_RECTS];

    /*
     * Producer damage is captured under window_lock, but expensive tile
     * expansion/planning is deliberately deferred until after the lock is
     * released.
     */
    bool damage_full_captured;
    bool damage_tiles_captured;
    uint64_t damage_tiles[WINDOW_DAMAGE_TILE_WORDS];

    /*
     * Current region being rendered.  Only the single active compositor
''',
        "snapshot captured-damage fields",
    )


# ============================================================
# P16.2 - tile bitmap -> rects from immutable snapshot, O(spans)
# ============================================================

# The old getter is used only by the old locked tile-expansion routine.
# P16 replaces that routine with an immutable-snapshot reader.
old_tile_getter_start = "static bool window_damage_tile_is_set_locked("
old_tile_getter_end = "static void window_damage_tile_set_locked("

if old_tile_getter_start in s and old_tile_getter_end in s:
    a = s.index(old_tile_getter_start)
    b = s.index(old_tile_getter_end, a)
    s = s[:a] + s[b:]


tile_start_marker = "/*\n * Convert overflow tile damage into horizontal spans"
tile_end_marker = "/*\n * Fast solid-color fill into the normal WB composite framebuffer."

if tile_start_marker not in s or tile_end_marker not in s:
    raise SystemExit("ERROR: tile conversion block markers not found")

tile_start = s.index(tile_start_marker)
tile_end = s.index(tile_end_marker, tile_start)

tile_block = r'''/*
 * Read one tile from the immutable producer bitmap captured for this frame.
 * No window_lock is required: the producer already moved on to a fresh damage
 * generation before this function runs.
 */
static bool compositor_snapshot_damage_tile_is_set(
    const compositor_snapshot_t *snapshot,
    uint32_t tile_x,
    uint32_t tile_y) {

    uint32_t index;

    if (snapshot == 0 ||
        tile_x >= WINDOW_DAMAGE_MAX_TILES_X ||
        tile_y >= WINDOW_DAMAGE_MAX_TILES_Y) {
        return false;
    }

    index =
        tile_y * WINDOW_DAMAGE_MAX_TILES_X +
        tile_x;

    return
        (snapshot->damage_tiles[index >> 6] &
         (1ULL << (index & 63U))) != 0U;
}


/*
 * Convert captured overflow tile damage into horizontal spans.
 *
 * Only an identical span from the immediately preceding tile row can merge
 * vertically with the current span.  Keeping two sorted active-span rows
 * changes the old "scan every rectangle already produced" behavior from
 * near O(total_spans^2) to O(total_spans).
 *
 * This entire routine runs outside window_lock.
 */
static uint32_t compositor_snapshot_tiles_to_rects(
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

    if (snapshot == 0 || capacity == 0U) {
        return 0U;
    }

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
                       snapshot, tile_x, tile_y)) {
                ++tile_x;
            }

            if (tile_x >= columns) {
                break;
            }

            start_x = tile_x;

            while (tile_x < columns &&
                   compositor_snapshot_damage_tile_is_set(
                       snapshot, tile_x, tile_y)) {
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
                    snapshot->damage_rects[index].bottom ==
                        tile_y * WINDOW_DAMAGE_TILE_SIZE) {

                    snapshot->damage_rects[index].bottom =
                        (tile_y + 1U) *
                        WINDOW_DAMAGE_TILE_SIZE;

                    if (snapshot->damage_rects[index].bottom >
                        g_window_server.display_height) {
                        snapshot->damage_rects[index].bottom =
                            g_window_server.display_height;
                    }

                    rect_index = index;
                }
            }

            if (rect_index == UINT32_MAX) {
                window_damage_rect_t candidate = {
                    .left =
                        start_x * WINDOW_DAMAGE_TILE_SIZE,

                    .top =
                        tile_y * WINDOW_DAMAGE_TILE_SIZE,

                    .right =
                        end_x * WINDOW_DAMAGE_TILE_SIZE,

                    .bottom =
                        (tile_y + 1U) *
                        WINDOW_DAMAGE_TILE_SIZE,
                };

                if (candidate.right >
                    g_window_server.display_width) {
                    candidate.right =
                        g_window_server.display_width;
                }

                if (candidate.bottom >
                    g_window_server.display_height) {
                    candidate.bottom =
                        g_window_server.display_height;
                }

                if (count < capacity) {
                    rect_index = count;
                    snapshot->damage_rects[count++] =
                        candidate;
                } else {
                    /*
                     * Preserve bounded memory use.  The final rectangle grows
                     * conservatively to contain every remaining span.
                     */
                    window_damage_rect_t *tail =
                        &snapshot->damage_rects[
                            capacity - 1U];

                    if (candidate.left < tail->left)
                        tail->left = candidate.left;

                    if (candidate.top < tail->top)
                        tail->top = candidate.top;

                    if (candidate.right > tail->right)
                        tail->right = candidate.right;

                    if (candidate.bottom > tail->bottom)
                        tail->bottom = candidate.bottom;
                }
            }

            if (rect_index != UINT32_MAX &&
                current_count <
                    (WINDOW_DAMAGE_MAX_TILES_X + 1U) / 2U) {

                current[current_count++] =
                    (active_span_t){
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


'''

s = s[:tile_start] + tile_block + s[tile_end:]


# ============================================================
# P16.3 - maintain one conservative producer damage bound
# ============================================================

s = replace_once(
    s,
    '''    if (!g_window_server.dirty) g_window_server.dirty = true;
    if (g_window_server.damage_full) return;

    window_damage_rect_t candidate = {
        .left = left,
        .top = top,
        .right = (uint32_t)right,
        .bottom = (uint32_t)bottom,
    };
''',
    '''    bool first_damage =
        !g_window_server.dirty;

    if (first_damage) {
        g_window_server.dirty = true;
    }

    if (g_window_server.damage_full) {
        return;
    }

    window_damage_rect_t candidate = {
        .left = left,
        .top = top,
        .right = (uint32_t)right,
        .bottom = (uint32_t)bottom,
    };

    /*
     * One conservative producer bound lets snapshot capture reject unrelated
     * windows with four comparisons, without expanding tile damage or scanning
     * the full rectangle set under window_lock.
     */
    if (first_damage) {
        g_window_server.damage_left =
            candidate.left;
        g_window_server.damage_top =
            candidate.top;
        g_window_server.damage_right =
            candidate.right;
        g_window_server.damage_bottom =
            candidate.bottom;
    } else {
        if (candidate.left <
            g_window_server.damage_left) {
            g_window_server.damage_left =
                candidate.left;
        }

        if (candidate.top <
            g_window_server.damage_top) {
            g_window_server.damage_top =
                candidate.top;
        }

        if (candidate.right >
            g_window_server.damage_right) {
            g_window_server.damage_right =
                candidate.right;
        }

        if (candidate.bottom >
            g_window_server.damage_bottom) {
            g_window_server.damage_bottom =
                candidate.bottom;
        }
    }
''',
    "producer damage bounds",
)

# Full-damage mode does not need to clear 2 KiB of stale tile state. Tile mode
# clears the bitmap before its next activation.
s = replace_once(
    s,
    '''    g_window_server.damage_count = 0U;
    window_damage_tiles_clear_locked();
    g_window_server.damage_left = 0U;
''',
    '''    g_window_server.damage_count = 0U;
    g_window_server.damage_left = 0U;
''',
    "full damage stale tile clear",
)


# ============================================================
# P16.4 - replace lock-heavy snapshot planning with capture+plan
# ============================================================

plan_start_marker = (
    "/*\n * Return true when a visible window can affect at least one rectangle"
)
plan_end_marker = "/*\n * Device-memory publication belongs to display core."

if plan_start_marker not in s or plan_end_marker not in s:
    raise SystemExit("ERROR: snapshot planning block markers not found")

plan_start = s.index(plan_start_marker)
plan_end = s.index(plan_end_marker, plan_start)

plan_block = r'''/*
 * Return true when one immutable window view can affect at least one final
 * damage rectangle.
 *
 * This is intentionally a snapshot-only function.  It runs after window_lock
 * has been released and therefore never touches mutable registry state.
 */
static bool compositor_view_intersects_snapshot_damage(
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

        const window_damage_rect_t *damage =
            &snapshot->damage_rects[index];

        if (damage->left >= damage->right ||
            damage->top >= damage->bottom) {
            continue;
        }

        if (right <= (int64_t)damage->left ||
            bottom <= (int64_t)damage->top ||
            left >= (int64_t)damage->right ||
            top >= (int64_t)damage->bottom) {
            continue;
        }

        return true;
    }

    return false;
}


/*
 * Collapse the immutable damage set to one conservative bounding rectangle.
 */
static bool compositor_snapshot_damage_bounds(
    const compositor_snapshot_t *snapshot,
    window_damage_rect_t *bounds) {

    bool have_bounds = false;

    if (snapshot == 0 || bounds == 0) {
        return false;
    }

    for (uint32_t index = 0U;
         index < snapshot->damage_count;
         ++index) {

        const window_damage_rect_t *rect =
            &snapshot->damage_rects[index];

        if (rect->left >= rect->right ||
            rect->top >= rect->bottom) {
            continue;
        }

        if (!have_bounds) {
            *bounds = *rect;
            have_bounds = true;
            continue;
        }

        if (rect->left < bounds->left)
            bounds->left = rect->left;

        if (rect->top < bounds->top)
            bounds->top = rect->top;

        if (rect->right > bounds->right)
            bounds->right = rect->right;

        if (rect->bottom > bounds->bottom)
            bounds->bottom = rect->bottom;
    }

    return have_bounds;
}


/*
 * Immutable-view equivalent of the old registry-side opaque-cover test.
 */
static bool compositor_view_fully_covers_snapshot_bounds(
    const compositor_window_view_t *window,
    const window_damage_rect_t *bounds) {

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

    opaque_left =
        frame_left + WINDOW_CORNER_RADIUS;

    opaque_top =
        frame_top + WINDOW_CORNER_RADIUS;

    opaque_right =
        frame_right - WINDOW_CORNER_RADIUS;

    opaque_bottom =
        frame_bottom - WINDOW_CORNER_RADIUS;

    return
        (int64_t)bounds->left >= opaque_left &&
        (int64_t)bounds->top >= opaque_top &&
        (int64_t)bounds->right <= opaque_right &&
        (int64_t)bounds->bottom <= opaque_bottom;
}


/*
 * Find the strongest Z-floor entirely from immutable views.
 *
 * snapshot->windows[] is still bottom -> top at this point.
 */
static uint32_t compositor_snapshot_occlusion_floor(
    const compositor_snapshot_t *snapshot) {

    window_damage_rect_t bounds;
    uint32_t floor = 0U;

    if (snapshot == 0 ||
        !compositor_snapshot_damage_bounds(
            snapshot, &bounds)) {
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


/*
 * Cheap producer-side candidate test.
 *
 * damage_left/top/right/bottom is one conservative bound maintained while
 * producers append rect/tile damage. It never creates false negatives.
 */
static bool compositor_registry_window_intersects_damage_bounds_locked(
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
        left < (int64_t)g_window_server.damage_right &&
        right > (int64_t)g_window_server.damage_left &&
        top < (int64_t)g_window_server.damage_bottom &&
        bottom > (int64_t)g_window_server.damage_top;
}


/*
 * P16 capture phase.
 *
 * Called with window_lock held. Expensive planning is forbidden here.
 */
static bool compositor_snapshot_begin_locked(void) {
    compositor_snapshot_t *snapshot =
        &g_compositor_snapshot;

    uint32_t region_count;

    if (!g_window_server.kernel_ready ||
        !g_window_server.dirty ||
        g_window_server.composing ||
        g_window_server.framebuffer == 0) {
        return false;
    }

    if (g_window_server.damage_full ||
        g_window_server.damage_tiles_active) {
        region_count = 1U;
    } else {
        region_count =
            g_window_server.damage_count;
    }

    if (region_count == 0U) {
        g_window_server.dirty = false;
        return false;
    }

    g_window_server.composing = true;

    snapshot->window_count = 0U;

    snapshot->focused_identifier =
        g_window_server.focused_identifier;

    snapshot->pointer_x =
        g_window_server.pointer_x;

    snapshot->pointer_y =
        g_window_server.pointer_y;

    snapshot->desktop_hovered_app =
        g_window_server.desktop_hovered_app;

    snapshot->dragging_identifier =
        g_window_server.dragging_identifier;

    snapshot->drag_blit_valid =
        g_window_server.drag_blit_valid;

    snapshot->drag_old_x =
        g_window_server.drag_old_x;

    snapshot->drag_old_y =
        g_window_server.drag_old_y;

    snapshot->drag_new_x =
        g_window_server.drag_new_x;

    snapshot->drag_new_y =
        g_window_server.drag_new_y;

    snapshot->drag_width =
        g_window_server.drag_width;

    snapshot->drag_height =
        g_window_server.drag_height;

    snapshot->damage_full_captured =
        g_window_server.damage_full;

    snapshot->damage_tiles_captured =
        !g_window_server.damage_full &&
        g_window_server.damage_tiles_active;

    if (snapshot->damage_full_captured) {
        snapshot->damage_count = 0U;
    } else if (snapshot->damage_tiles_captured) {
        snapshot->damage_count = 0U;

        for (uint32_t word = 0U;
             word < WINDOW_DAMAGE_TILE_WORDS;
             ++word) {
            snapshot->damage_tiles[word] =
                g_window_server.damage_tiles[word];
        }
    } else {
        snapshot->damage_count =
            g_window_server.damage_count;

        for (uint32_t index = 0U;
             index < snapshot->damage_count;
             ++index) {
            snapshot->damage_rects[index] =
                g_window_server.damage_rects[index];
        }
    }

    /*
     * Capture visible windows once, in Z order. The object reference makes
     * every copied section pointer stable after the lock is released.
     */
    for (uint32_t index = 0U;
         index < g_window_server.count;
         ++index) {

        window_server_window_t *window =
            g_window_server.windows[index];

        compositor_window_view_t *view;

        if (window == 0 ||
            window->minimized ||
            (window->flags & OS_WINDOW_VISIBLE) == 0U) {
            continue;
        }

        /*
         * Keep the dragged window for committed-geometry bookkeeping even if
         * it has just moved fully off-screen. Every other window must at least
         * intersect the conservative producer damage bound.
         */
        if (window->identifier !=
                snapshot->dragging_identifier &&
            !compositor_registry_window_intersects_damage_bounds_locked(
                window)) {
            continue;
        }

        if (snapshot->window_count >=
            WINDOW_SERVER_MAX_WINDOWS) {
            break;
        }

        object_get(window);

        view =
            &snapshot->windows[
                snapshot->window_count++];

        view->reference = window;
        view->section = window->section;
        view->identifier = window->identifier;
        view->x = window->x;
        view->y = window->y;
        view->width = window->width;
        view->height = window->height;
        view->flags = window->flags;
        view->background = window->background;
        view->buffer_size = window->buffer_size;
        view->owner_address = window->owner_address;
        view->resize_pending = window->resize_pending;
        view->maximized = window->maximized;

        for (uint32_t title_index = 0U;
             title_index < sizeof(view->title);
             ++title_index) {

            view->title[title_index] =
                window->title[title_index];

            if (window->title[title_index] ==
                '\0') {
                break;
            }
        }

        view->title[
            sizeof(view->title) - 1U] =
            '\0';
    }

    /*
     * Rotate producer state immediately.
     *
     * Do not clear damage_tiles[] here. The bitmap is unreachable once
     * damage_tiles_active becomes false, and enable_tiles clears it before
     * the next reuse. This removes 256 stores from the lock section.
     */
    g_window_server.damage_count = 0U;
    g_window_server.damage_full = false;
    g_window_server.damage_tiles_active = false;
    g_window_server.dirty = false;

    g_window_server.damage_left =
        g_window_server.display_width;

    g_window_server.damage_top =
        g_window_server.display_height;

    g_window_server.damage_right = 0U;
    g_window_server.damage_bottom = 0U;

    return true;
}


/*
 * P16 planning phase. Called without window_lock.
 */
static void compositor_snapshot_plan(void) {
    compositor_snapshot_t *snapshot =
        &g_compositor_snapshot;

    uint32_t original_window_count;
    uint32_t first_window_index;
    uint32_t write_index = 0U;

    if (snapshot->damage_full_captured) {
        snapshot->damage_count = 1U;

        snapshot->damage_rects[0] =
            (window_damage_rect_t){
                .left = 0U,
                .top = 0U,
                .right =
                    g_window_server.display_width,
                .bottom =
                    g_window_server.display_height,
            };

    } else if (snapshot->damage_tiles_captured) {

        snapshot->damage_count =
            compositor_snapshot_tiles_to_rects(
                snapshot,
                WINDOW_DAMAGE_MAX_SNAPSHOT_RECTS);
    }

    if (snapshot->damage_count == 0U) {
        snapshot->damage_count = 1U;

        snapshot->damage_rects[0] =
            (window_damage_rect_t){
                .left = 0U,
                .top = 0U,
                .right =
                    g_window_server.display_width,
                .bottom =
                    g_window_server.display_height,
            };
    }

    if (!snapshot->drag_blit_valid) {
        compositor_collapse_drag_damage(
            snapshot);
    }

    first_window_index =
        compositor_snapshot_occlusion_floor(
            snapshot);

    original_window_count =
        snapshot->window_count;

    for (uint32_t index = 0U;
         index < original_window_count;
         ++index) {

        compositor_window_view_t *view =
            &snapshot->windows[index];

        bool keep =
            index >= first_window_index &&
            (view->identifier ==
                 snapshot->dragging_identifier ||
             compositor_view_intersects_snapshot_damage(
                 view, snapshot));

        if (!keep) {
            if (view->reference != 0) {
                object_put(view->reference);
                view->reference = 0;
            }

            view->section = 0;
            continue;
        }

        if (write_index != index) {
            snapshot->windows[write_index] =
                *view;

            view->reference = 0;
            view->section = 0;
        }

        ++write_index;
    }

    snapshot->window_count =
        write_index;

    snapshot->damage_full_captured =
        false;

    snapshot->damage_tiles_captured =
        false;
}


'''

s = s[:plan_start] + plan_block + s[plan_end:]


# ============================================================
# P16.5 - planner runs after dropping window_lock
# ============================================================

s = replace_once(
    s,
    '''    window_unlock();

    if (compose) {
        compositor_render_snapshot();
        compositor_snapshot_finish();
    }
}
''',
    '''    window_unlock();

    if (compose) {
        compositor_snapshot_plan();
        compositor_render_snapshot();
        compositor_snapshot_finish();
    }
}
''',
    "compose planner call",
)


# ============================================================
# P17.1 - per-window event wake bookkeeping
# ============================================================

if "event_wake_pending" not in h:
    h = replace_once(
        h,
        '''    uint32_t event_read;
    uint32_t event_write;
    uint32_t event_count;
    wait_queue_t event_waitq;
''',
        '''    uint32_t event_read;
    uint32_t event_write;
    uint32_t event_count;

    /* Protected by the window-server lock. */
    bool event_wake_pending;

    wait_queue_t event_waitq;
''',
        "window event wake field",
    )


if "event_ready_windows[WINDOW_SERVER_MAX_WINDOWS]" not in s:
    s = replace_once(
        s,
        '''    wait_queue_t event_waitq;
    wait_queue_t worker_waitq;
    atomic_uint_fast64_t worker_generation;
    window_server_window_t *windows[WINDOW_SERVER_MAX_WINDOWS];
''',
        '''    wait_queue_t event_waitq;
    wait_queue_t worker_waitq;
    atomic_uint_fast64_t worker_generation;

    window_server_window_t *
        event_ready_windows[WINDOW_SERVER_MAX_WINDOWS];

    uint32_t event_ready_count;
    bool event_ready_overflow;

    window_server_window_t *windows[WINDOW_SERVER_MAX_WINDOWS];
''',
        "global event ready set",
    )


s = replace_once(
    s,
    '''        g_window_server.count = 0U;
        g_window_server.next_identifier = 1U;
''',
    '''        g_window_server.event_ready_count = 0U;
        g_window_server.event_ready_overflow = false;

        for (uint32_t index = 0U;
             index < WINDOW_SERVER_MAX_WINDOWS;
             ++index) {
            g_window_server.event_ready_windows[index] = 0;
        }

        g_window_server.count = 0U;
        g_window_server.next_identifier = 1U;
''',
    "event ready init",
)


s = replace_once(
    s,
    '''    window->event_read = 0U;
    window->event_write = 0U;
    window->event_count = 0U;
    wait_queue_init(&window->event_waitq);
''',
    '''    window->event_read = 0U;
    window->event_write = 0U;
    window->event_count = 0U;
    window->event_wake_pending = false;
    wait_queue_init(&window->event_waitq);
''',
    "window event wake init",
)


# ============================================================
# P17.2 - O(k) producer ready set
# ============================================================

event_insert_marker = (
    "static void window_enqueue_event_locked(window_server_window_t *window,"
)

if event_insert_marker not in s:
    raise SystemExit("ERROR: event enqueue insertion marker missing")

event_helpers = r'''/*
 * Record one window for a post-lock waiter wake.
 *
 * Normal path is O(1). A temporary object reference keeps the embedded wait
 * queue alive across concurrent close/process teardown.
 */
static void window_event_schedule_wake_locked(
    window_server_window_t *window) {

    if (window == 0 ||
        window->event_wake_pending) {
        return;
    }

    if (g_window_server.event_ready_count >=
        WINDOW_SERVER_MAX_WINDOWS) {

        g_window_server.event_ready_overflow =
            true;

        return;
    }

    object_get(window);

    window->event_wake_pending = true;

    g_window_server.event_ready_windows[
        g_window_server.event_ready_count++] =
        window;
}


/*
 * Detach the ready set under window_lock, then perform wake_one/object_put
 * outside the lock.
 *
 * Only the extremely rare overflow path scans the registry.
 */
static bool window_flush_event_wakes(void) {
    window_server_window_t *
        ready[WINDOW_SERVER_MAX_WINDOWS];

    window_server_window_t *
        overflow_ready[WINDOW_SERVER_MAX_WINDOWS];

    uint32_t ready_count = 0U;
    uint32_t overflow_count = 0U;
    bool overflow;

    window_lock();

    ready_count =
        g_window_server.event_ready_count;

    if (ready_count >
        WINDOW_SERVER_MAX_WINDOWS) {
        ready_count =
            WINDOW_SERVER_MAX_WINDOWS;
    }

    for (uint32_t index = 0U;
         index < ready_count;
         ++index) {

        window_server_window_t *window =
            g_window_server.event_ready_windows[index];

        ready[index] = window;

        g_window_server.event_ready_windows[index] = 0;

        if (window != 0) {
            window->event_wake_pending = false;
        }
    }

    g_window_server.event_ready_count = 0U;

    overflow =
        g_window_server.event_ready_overflow;

    g_window_server.event_ready_overflow =
        false;

    if (overflow) {
        for (uint32_t index = 0U;
             index < g_window_server.count &&
             overflow_count < WINDOW_SERVER_MAX_WINDOWS;
             ++index) {

            window_server_window_t *window =
                g_window_server.windows[index];

            if (window == 0 ||
                window->event_count == 0U) {
                continue;
            }

            object_get(window);

            overflow_ready[
                overflow_count++] =
                window;
        }
    }

    window_unlock();

    for (uint32_t index = 0U;
         index < ready_count;
         ++index) {

        if (ready[index] == 0) {
            continue;
        }

        (void)wake_one(
            &ready[index]->event_waitq);

        object_put(
            ready[index]);
    }

    for (uint32_t index = 0U;
         index < overflow_count;
         ++index) {

        (void)wake_one(
            &overflow_ready[index]->event_waitq);

        object_put(
            overflow_ready[index]);
    }

    return
        ready_count != 0U ||
        overflow_count != 0U;
}


'''

insert_at = s.index(event_insert_marker)
s = s[:insert_at] + event_helpers + s[insert_at:]


# Schedule a specific wake after every actual queue insertion.
enqueue_increment = "    ++window->event_count;\n"
enqueue_count = s.count(enqueue_increment)

if enqueue_count < 3:
    raise SystemExit(
        f"ERROR: expected >=3 event enqueue increments, found {enqueue_count}"
    )

s = s.replace(
    enqueue_increment,
    '''    ++window->event_count;
    window_event_schedule_wake_locked(window);
''',
)


# Remove old O(N) wake scan.
wake_scan_start = (
    "/* Wake per-window readers only after releasing window_lock."
)
wake_scan_end = (
    "kstatus_t window_server_register_manager(process_t *process)"
)

if wake_scan_start not in s or wake_scan_end not in s:
    raise SystemExit("ERROR: old wake scan block not found")

a = s.index(wake_scan_start)
b = s.index(wake_scan_end, a)

s = s[:a] + s[b:]


# Direct dispatch: O(1) normal wake path.
s = replace_once(
    s,
    '''    window_unlock();
    (void)wake_all(&g_window_server.event_waitq);
    window_wake_ready_event_waiters();
    return K_OK;
}
''',
    '''    window_unlock();

    if (window_flush_event_wakes()) {
        (void)wake_all(
            &g_window_server.event_waitq);
    }

    return K_OK;
}
''',
    "dispatch wake flush",
)


# Input pump: desktop-only motion no longer wakes all Ring3 waiters.
s = replace_once(
    s,
    '''    if (wake) {
        (void)wake_all(&g_window_server.event_waitq);
        window_wake_ready_event_waiters();
    }
''',
    '''    if (wake &&
        window_flush_event_wakes()) {

        (void)wake_all(
            &g_window_server.event_waitq);
    }
''',
    "input pump wake flush",
)


SRC.write_text(s, encoding="utf-8")
HDR.write_text(h, encoding="utf-8")

print("OK: P16 + P17 window-system optimization installed")
print(f"source backup: {src_backup}")
print(f"header backup: {hdr_backup}")
