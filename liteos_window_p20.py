#!/usr/bin/env python3
from pathlib import Path

SRC = Path("kernel/graphics/window_server.c")
s = SRC.read_text(encoding="utf-8")

if "g_compositor_render_plan" not in s:
    raise SystemExit(
        "ERROR: P20 expects P19 to be applied first. "
        "Run liteos_window_p19.py before this script."
    )

if "window_mask" in s and "compositor_draw_window_locked" in s:
    print("P20 per-span window-mask command stream already installed")
    raise SystemExit(0)

backup = SRC.with_suffix(".c.before-p20")
if not backup.exists():
    backup.write_text(s, encoding="utf-8")


def replace_once(text, old, new, name):
    if old not in text:
        raise SystemExit(f"ERROR: {name}: anchor not found")
    return text.replace(old, new, 1)


# ============================================================
# P20.1 - hard architectural invariant: one u64 == one snapshot Z set
# ============================================================

anchor = '''#define WINDOW_RENDER_PLAN_MAX_SPANS 4096U

#define WINDOW_SERVER_WORKER_STACK_SIZE (64U * 1024U)
'''

replacement = '''#define WINDOW_RENDER_PLAN_MAX_SPANS 4096U

/*
 * P20 stores all windows that can contribute to one RenderSpan in one u64.
 * Keep this explicit so a future increase of WINDOW_SERVER_MAX_WINDOWS cannot
 * silently truncate the compositor command stream.
 */
_Static_assert(WINDOW_SERVER_MAX_WINDOWS <= 64U,
               "P20 window mask requires <= 64 windows");

#define WINDOW_SERVER_WORKER_STACK_SIZE (64U * 1024U)
'''

s = replace_once(
    s, anchor, replacement,
    "P20 u64 window-mask invariant"
)


# ============================================================
# P20.2 - augment each P19 span with the executable command mask
# ============================================================

old_struct = '''typedef struct compositor_render_span {
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
'''

new_struct = '''typedef struct compositor_render_span {
    window_damage_rect_t rect;

    /*
     * P19 spatial floor used while adjacent tile spans are being merged.
     *
     * UINT32_MAX means this span still requires desktop/below-window pixels.
     */
    uint32_t first_window;

    /*
     * P20 executable command stream:
     *
     * bit N == snapshot->windows[N] actually intersects this span and must be
     * drawn.  Rendering consumes this with one bit-scan loop instead of
     * scanning first_window..window_count and re-running geometry tests.
     */
    uint64_t window_mask;

    bool desktop_required;
} compositor_render_span_t;
'''

s = replace_once(
    s, old_struct, new_struct,
    "P20 render-span command fields"
)


# ============================================================
# P20.3 - exact immutable per-span intersection + mask compiler
# ============================================================

marker = '''/*
 * Build one immutable render plan from snapshot damage + P18 tile floors.
'''

if marker not in s:
    raise SystemExit("ERROR: P19 render-plan builder marker not found")

helpers = r'''/*
 * Exact immutable outer-bounds intersection for one final RenderSpan.
 *
 * The P16 snapshot already copied all geometry needed here, so compiling the
 * command mask touches no mutable window state and needs no window_lock.
 */
static bool compositor_view_intersects_render_rect(
    const compositor_window_view_t *window,
    const window_damage_rect_t *rect) {

    int64_t left;
    int64_t top;
    int64_t right;
    int64_t bottom;

    if (window == 0 || rect == 0 ||
        rect->left >= rect->right ||
        rect->top >= rect->bottom) {
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
        right > (int64_t)rect->left &&
        bottom > (int64_t)rect->top &&
        left < (int64_t)rect->right &&
        top < (int64_t)rect->bottom;
}


/*
 * Compile one spatial span into one 64-bit display-list mask.
 *
 * P19 has already proved that windows below first_window are irrelevant when
 * first_window != UINT32_MAX.  We only test the remaining immutable views.
 */
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

    for (uint32_t index = first;
         index < snapshot->window_count;
         ++index) {

        if (!compositor_view_intersects_render_rect(
                &snapshot->windows[index],
                &span->rect)) {
            continue;
        }

        mask |=
            1ULL << index;
    }

    return mask;
}


/*
 * Final P20 compiler pass.
 *
 * This converts P19's spatial plan into an executable display-list-like
 * command stream.  All geometry tests happen once here; the render phase only
 * performs bit scans and leaf drawing.
 */
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


'''

insert_at = s.index(marker)
s = s[:insert_at] + helpers + s[insert_at:]


# ============================================================
# P20.4 - compile masks only after P19 successfully built the plan
# ============================================================

old_tail = '''    g_compositor_render_plan_valid =
        true;

    return true;
}
'''

new_tail = '''    g_compositor_render_plan_valid =
        true;

    /*
     * P20: compile exact window-intersection masks only after P19 has finished
     * all horizontal/vertical span merging.  This avoids recomputing masks for
     * temporary tile fragments that later collapse into one rectangle.
     */
    compositor_compile_render_plan_commands(
        snapshot);

    return true;
}
'''

s = replace_once(
    s, old_tail, new_tail,
    "P20 command compiler call"
)


# ============================================================
# P20.5 - make compositor_region consume the complete span command
# ============================================================

old_sig = '''static void compositor_region_locked(
    uint32_t planned_first_window,
    bool planned) {
'''

new_sig = '''static void compositor_region_locked(
    const compositor_render_span_t *planned_span) {
'''

s = replace_once(
    s, old_sig, new_sig,
    "P20 region signature"
)


old_floor = '''    uint32_t first_window;

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

new_floor = '''    uint32_t first_window = 0U;

    if (planned_span != 0) {
        /*
         * P20 plan path: whether the desktop is required was decided before
         * rendering.  No occlusion query or reverse Z scan remains here.
         */
        if (planned_span->desktop_required) {
            desktop_draw_wallpaper_locked();
        }
    } else {
        /*
         * Simple frame / plan-overflow fallback retains P18 exactly.
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
    s, old_floor, new_floor,
    "P20 region desktop/fallback logic"
)


# ============================================================
# P20.6 - extract one leaf draw operation
# ============================================================

region_marker = '''static void compositor_region_locked(
    const compositor_render_span_t *planned_span) {
'''

if region_marker not in s:
    raise SystemExit("ERROR: patched compositor_region_locked marker missing")

draw_helper = r'''/*
 * Draw one already-selected immutable window for the current damage rectangle.
 *
 * Both the P20 command-mask path and the old P18 fallback converge here, so
 * titlebar/surface semantics remain exactly identical.
 */
static inline void compositor_draw_window_locked(
    const compositor_window_view_t *window) {

    uint32_t frame_color;

    if (window == 0) {
        return;
    }

    frame_color =
        window->identifier ==
        g_compositor_snapshot.focused_identifier ?
            0x005F8FC4U :
            0x0028323EU;

    if (!window_client_decorations(window->flags) &&
        !compositor_damage_inside_surface_interior(window)) {
        compositor_titlebar_locked(
            window,
            frame_color);
    }

    compositor_surface_locked(
        window);
}


'''

insert_at = s.index(region_marker)
s = s[:insert_at] + draw_helper + s[insert_at:]


# ============================================================
# P20.7 - replace candidate-window scan with mask bit-scan
# ============================================================

old_loop = '''    for (uint32_t index = first_window; index < g_compositor_snapshot.window_count; ++index) {

        const compositor_window_view_t *window =
            &g_compositor_snapshot.windows[index];

        uint32_t frame_color;

        if ((window->flags & OS_WINDOW_VISIBLE) == 0U ||
            !compositor_window_intersects_damage_locked(window)) {
            continue;
        }

        frame_color =
            window->identifier ==
            g_compositor_snapshot.focused_identifier ?
                0x005F8FC4U :
                0x0028323EU;

        if (!window_client_decorations(window->flags) &&
            !compositor_damage_inside_surface_interior(window)) {
            compositor_titlebar_locked(window, frame_color);
        }

        compositor_surface_locked(window);
    }
'''

new_loop = '''    if (planned_span != 0) {
        /*
         * P20 hot path.
         *
         * GCC/Clang lower __builtin_ctzll() to TZCNT when BMI is enabled or
         * BSF otherwise.  mask &= mask-1 removes the selected window bit.
         * Z order is preserved because we always consume the least set bit.
         */
        uint64_t mask =
            planned_span->window_mask;

        while (mask != 0U) {
            uint32_t index =
                (uint32_t)__builtin_ctzll(mask);

            mask &= mask - 1U;

            if (index >=
                g_compositor_snapshot.window_count) {
                continue;
            }

            compositor_draw_window_locked(
                &g_compositor_snapshot.windows[index]);
        }

    } else {
        /*
         * P18 fallback: only simple/pathological frames still execute the
         * candidate scan and per-window geometry intersection here.
         */
        for (uint32_t index = first_window;
             index < g_compositor_snapshot.window_count;
             ++index) {

            const compositor_window_view_t *window =
                &g_compositor_snapshot.windows[index];

            if ((window->flags & OS_WINDOW_VISIBLE) == 0U ||
                !compositor_window_intersects_damage_locked(
                    window)) {
                continue;
            }

            compositor_draw_window_locked(
                window);
        }
    }
'''

s = replace_once(
    s, old_loop, new_loop,
    "P20 bit-scan render loop"
)


# ============================================================
# P20.8 - planned/fallback call sites
# ============================================================

old_planned_call = '''            compositor_region_locked(
                span->first_window,
                true);
'''

new_planned_call = '''            compositor_region_locked(
                span);
'''

s = replace_once(
    s, old_planned_call, new_planned_call,
    "P20 planned call site"
)


old_fallback_call = '''            compositor_region_locked(
                UINT32_MAX,
                false);
'''

new_fallback_call = '''            compositor_region_locked(
                0);
'''

s = replace_once(
    s, old_fallback_call, new_fallback_call,
    "P20 fallback call site"
)


SRC.write_text(s, encoding="utf-8")

print("OK: P20 per-span u64 window-mask command stream installed")
print(f"backup: {backup}")
