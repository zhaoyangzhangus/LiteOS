#!/usr/bin/env python3
from pathlib import Path
import argparse
import shutil
import subprocess
import sys

EXPECTED_HEAD = "40c242b066fc1a896f0933616cd20f9b749d2ae1"
TARGET = Path("kernel/graphics/window_server.c")
V1_MARKER = "LITEOS_COMPOSITOR_OPT_V1"
V2_MARKER = "LITEOS_COMPOSITOR_RETAINED_DRAG_V2"

def die(message):
    print(f"ERROR: {message}", file=sys.stderr)
    raise SystemExit(1)

def git(repo, *args, check=True):
    result = subprocess.run(
        ["git", "-C", str(repo), *args],
        text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    if check and result.returncode != 0:
        die(result.stderr.strip() or f"git {' '.join(args)} failed")
    return result

def git_head(repo):
    return git(repo, "rev-parse", "HEAD").stdout.strip()

def replace_once(text, old, new, label):
    count = text.count(old)
    if count != 1:
        die(f"{label}: expected exactly one anchor, found {count}")
    return text.replace(old, new, 1)

def insert_before_once(text, anchor, insertion, label):
    count = text.count(anchor)
    if count != 1:
        die(f"{label}: expected exactly one anchor, found {count}")
    return text.replace(anchor, insertion + anchor, 1)

def replace_between(text, start_anchor, end_anchor, replacement, label):
    start = text.find(start_anchor)
    if start < 0:
        die(f"{label}: start anchor not found")
    end = text.find(end_anchor, start + len(start_anchor))
    if end < 0:
        die(f"{label}: end anchor not found")
    if text.find(start_anchor, start + 1) >= 0:
        die(f"{label}: start anchor not unique")
    return text[:start] + replacement + text[end:]

def backup_v2(repo):
    backup = repo / f".compositor-v2-backup-{EXPECTED_HEAD[:8]}" / TARGET
    if not backup.exists():
        backup.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(repo / TARGET, backup)
    return backup

def restore_v1_if_needed(repo, text):
    if V1_MARKER not in text:
        return text, False

    backup = repo / f".compositor-opt-backup-{EXPECTED_HEAD[:8]}" / TARGET
    if not backup.exists():
        die(
            "v1 marker detected, but its pristine backup is missing:\n"
            f"  {backup}\n"
            "Run the v1 rollback first, or restore window_server.c from "
            f"{EXPECTED_HEAD}."
        )

    pristine = backup.read_text(encoding="utf-8")
    if V1_MARKER in pristine:
        die("v1 backup itself contains the v1 marker; refusing to stack patches")

    shutil.copy2(backup, repo / TARGET)
    print(f"Restored pristine window_server.c from v1 backup: {backup}")
    return pristine, True

def patch_constants(text):
    old = '''#define WINDOW_COMPOSITOR_PUBLICATION_ROW_CHUNK 16U
#define WINDOW_COMPOSITOR_ATOMIC_PIXELS (32U * 1024U)
#define WINDOW_COMPOSITOR_COPY_MAX_WORKERS 3U
#define WINDOW_COMPOSITOR_COPY_STACK_SIZE (32U * 1024U)
'''
    new = f'''#define WINDOW_COMPOSITOR_PUBLICATION_ROW_CHUNK 16U
#define WINDOW_COMPOSITOR_ATOMIC_PIXELS (32U * 1024U)

/* {V2_MARKER}
 *
 * Retained-drag is the primary optimization in v2.  Cross-CPU publication is
 * kept only for large rectangles so small updates do not pay wake/IPI costs.
 */
#ifndef WINDOW_COMPOSITOR_PARALLEL_PIXELS
#define WINDOW_COMPOSITOR_PARALLEL_PIXELS (512U * 1024U)
#endif
#define WINDOW_COMPOSITOR_COPY_MAX_WORKERS 2U
#define WINDOW_COMPOSITOR_COPY_STACK_SIZE (32U * 1024U)

/* WB desktop-cache -> WB retained-scene copies must stay cached. */
#ifndef WINDOW_COMPOSITOR_PARALLEL_WB_COPY
#define WINDOW_COMPOSITOR_PARALLEL_WB_COPY 0U
#endif
'''
    return replace_once(text, old, new, "constants")

def patch_state(text):
    old = '''    uint32_t pointer_x;
    uint32_t pointer_y;
    uint32_t desktop_hovered_app;
'''
    new = '''    uint32_t pointer_x;
    uint32_t pointer_y;

    /* The WB composite buffer is a clean reusable scene and never contains
     * the software cursor.  These fields track the cursor actually overlaid
     * on the WC scanout so pointer motion can be presented independently of
     * the 60Hz scene scheduler. */
    uint32_t presented_pointer_x;
    uint32_t presented_pointer_y;
    bool presented_pointer_valid;

    uint32_t desktop_hovered_app;
'''
    text = replace_once(text, old, new, "presented cursor state")

    old = '''        g_window_server.pointer_x = 0U;
        g_window_server.pointer_y = 0U;
        g_window_server.desktop_hovered_app = DESKTOP_APP_NONE;
'''
    new = '''        g_window_server.pointer_x = 0U;
        g_window_server.pointer_y = 0U;
        g_window_server.presented_pointer_x = 0U;
        g_window_server.presented_pointer_y = 0U;
        g_window_server.presented_pointer_valid = false;
        g_window_server.desktop_hovered_app = DESKTOP_APP_NONE;
'''
    text = replace_once(text, old, new, "presented cursor init")

    old = '''static void window_server_pump_input_mode(bool compose_now);
static bool window_server_start_copy_workers(uint32_t compositor_cpu);
'''
    new = '''static void window_server_pump_input_mode(bool compose_now);
static void compositor_present_cursor_direct(bool force);
static bool window_server_start_copy_workers(uint32_t compositor_cpu);
'''
    text = replace_once(text, old, new, "cursor prototype")
    return text

def patch_worker_immediate_cursor(text):
    old = '''            if (input_core_pending() != 0U) {
                window_server_pump_input_mode(false);
            }
'''
    new = '''            if (input_core_pending() != 0U) {
                window_server_pump_input_mode(false);

                /*
                 * Pointer state is processed immediately, while normal scene
                 * composition remains frame-paced.  The retained WB scene is
                 * cursor-free, so restoring the previous 24x24 cursor and
                 * overlaying the new one is cheap and safe at HID rate.
                 */
                compositor_present_cursor_direct(false);
            }
'''
    return replace_once(text, old, new, "worker immediate cursor")

def patch_drag_helpers(text):
    anchor = '''static uint32_t window_damage_tile_columns_locked(void) {
'''
    helper = r'''static bool window_drag_reuse_available_locked(
    const window_server_window_t *window) {

    return window != 0 &&
           g_window_server.resize_edges == 0U &&
           g_window_server.composite_framebuffer != 0 &&
           g_window_server.composite_framebuffer !=
               g_window_server.framebuffer &&
           g_window_server.count != 0U &&
           g_window_server.windows[g_window_server.count - 1U] == window;
}


'''
    text = insert_before_once(text, anchor, helper, "drag reuse helper")

    implementation_anchor = '''static void window_mark_moved_cursor_locked(uint32_t old_x, uint32_t old_y,
'''
    helper2 = r'''static void window_mark_drag_corner_repair_locked(
    int32_t x,
    int32_t y,
    uint32_t outer_width,
    uint32_t outer_height,
    uint32_t flags) {

    uint32_t radius = WINDOW_CORNER_RADIUS;

    if (window_client_decorations(flags) ||
        outer_width == 0U || outer_height == 0U) {
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


'''
    first = text.find(implementation_anchor)
    if first < 0:
        die("cursor marker declaration not found")
    second = text.find(implementation_anchor, first + len(implementation_anchor))
    if second < 0:
        die("cursor marker implementation not found")
    text = text[:second] + helper2 + text[second:]
    return text

def patch_cursor_damage(text):
    start = '''static void window_mark_moved_cursor_locked(uint32_t old_x, uint32_t old_y,
                                            uint32_t new_x, uint32_t new_y) {
'''
    first = text.find(start)
    if first < 0:
        die("cursor damage implementation not found")
    body_pos = first + len(start)
    insertion = '''    if (old_x == new_x && old_y == new_y) {
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

'''
    return text[:body_pos] + insertion + text[body_pos:]

def patch_cursor_scene_and_direct_publish(text):
    old = '''    compositor_cursor_locked();

    /*
     * Do not expose this region to the scanout yet.
'''
    new = '''    /*
     * Private WB composition must remain cursor-free so its pixels can be
     * reused by retained drag.  The low-memory direct-framebuffer fallback
     * cannot retain a clean scene, so keep the legacy in-scene cursor there.
     */
    if (g_window_server.composite_framebuffer ==
        g_window_server.framebuffer) {
        compositor_cursor_locked();
    }

    /*
     * Do not expose this region to the scanout yet.
'''
    text = replace_once(text, old, new, "cursor-free retained scene")

    anchor = '''


/*
 * Return true only when the COMPLETE current damage rectangle lies inside an
'''
    helper = r'''

/*
 * Overlay the software cursor directly on the WC scanout while keeping the
 * retained WB compositor buffer clean.
 */
static void compositor_present_cursor_direct(bool force) {
    uint32_t pointer_x;
    uint32_t pointer_y;
    int64_t old_origin_x;
    int64_t old_origin_y;
    int64_t new_origin_x;
    int64_t new_origin_y;

    if (g_window_server.framebuffer == 0 ||
        g_window_server.composite_framebuffer == 0 ||
        g_window_server.composite_framebuffer ==
            g_window_server.framebuffer ||
        g_window_server.display_width == 0U ||
        g_window_server.display_height == 0U) {
        return;
    }

    pointer_x = g_window_server.pointer_x;
    pointer_y = g_window_server.pointer_y;

    if (!force &&
        g_window_server.presented_pointer_valid &&
        g_window_server.presented_pointer_x == pointer_x &&
        g_window_server.presented_pointer_y == pointer_y) {
        return;
    }

    sched_preempt_disable();

    if (g_window_server.presented_pointer_valid) {
        old_origin_x =
            (int64_t)g_window_server.presented_pointer_x -
            WINDOW_CURSOR_HOTSPOT_X;
        old_origin_y =
            (int64_t)g_window_server.presented_pointer_y -
            WINDOW_CURSOR_HOTSPOT_Y;

        for (uint32_t row = 0U; row < WINDOW_CURSOR_HEIGHT; ++row) {
            int64_t y = old_origin_y + (int64_t)row;
            if (y < 0 ||
                y >= (int64_t)g_window_server.display_height) {
                continue;
            }

            for (uint32_t column = 0U;
                 column < WINDOW_CURSOR_WIDTH;
                 ++column) {
                int64_t x = old_origin_x + (int64_t)column;
                uint64_t offset;

                if (x < 0 ||
                    x >= (int64_t)g_window_server.display_width) {
                    continue;
                }

                offset =
                    (uint64_t)y * g_window_server.display_stride +
                    (uint32_t)x;

                g_window_server.framebuffer[offset] =
                    g_window_server.composite_framebuffer[offset];
            }
        }
    }

    new_origin_x =
        (int64_t)pointer_x - WINDOW_CURSOR_HOTSPOT_X;
    new_origin_y =
        (int64_t)pointer_y - WINDOW_CURSOR_HOTSPOT_Y;

    for (uint32_t row = 0U; row < WINDOW_CURSOR_HEIGHT; ++row) {
        int64_t y = new_origin_y + (int64_t)row;
        if (y < 0 ||
            y >= (int64_t)g_window_server.display_height) {
            continue;
        }

        for (uint32_t column = 0U;
             column < WINDOW_CURSOR_WIDTH;
             ++column) {
            int64_t x = new_origin_x + (int64_t)column;
            uint32_t cursor;
            uint32_t alpha;
            uint32_t background;
            uint32_t output;
            uint64_t offset;

            if (x < 0 ||
                x >= (int64_t)g_window_server.display_width) {
                continue;
            }

            offset =
                (uint64_t)y * g_window_server.display_stride +
                (uint32_t)x;

            background =
                g_window_server.composite_framebuffer[offset];

            cursor =
                g_linux_cursor_argb[
                    row * WINDOW_CURSOR_WIDTH + column];

            alpha = cursor >> 24;

            if (alpha == 0U) {
                output = background;
            } else if (alpha >= 255U) {
                output = cursor & 0x00FFFFFFU;
            } else {
                uint32_t color = cursor & 0x00FFFFFFU;
                uint32_t inverse = 255U - alpha;

                uint32_t red =
                    ((((background >> 16) & 0xFFU) * inverse) +
                     (((color >> 16) & 0xFFU) * alpha) +
                     127U) / 255U;

                uint32_t green =
                    ((((background >> 8) & 0xFFU) * inverse) +
                     (((color >> 8) & 0xFFU) * alpha) +
                     127U) / 255U;

                uint32_t blue =
                    (((background & 0xFFU) * inverse) +
                     ((color & 0xFFU) * alpha) +
                     127U) / 255U;

                output =
                    (red << 16) |
                    (green << 8) |
                    blue;
            }

            g_window_server.framebuffer[offset] = output;
        }
    }

    __asm__ volatile ("sfence" : : : "memory");

    g_window_server.presented_pointer_x = pointer_x;
    g_window_server.presented_pointer_y = pointer_y;
    g_window_server.presented_pointer_valid = true;

    sched_preempt_enable();
}

'''
    return replace_once(text, anchor, helper + anchor, "direct cursor publisher")

def patch_drag_blit_function(text):
    start = '''static void compositor_blit_drag_overlap(void) {
'''
    end = '''static void compositor_render_snapshot(void) {
'''
    replacement = r'''static void compositor_blit_drag_overlap(void) {
    const compositor_snapshot_t *snapshot = &g_compositor_snapshot;
    int64_t local_left;
    int64_t local_top;
    int64_t local_right;
    int64_t local_bottom;
    uint32_t width;
    uint32_t height;

    if (!snapshot->drag_blit_valid ||
        snapshot->dragging_identifier == 0U ||
        snapshot->drag_width == 0U ||
        snapshot->drag_height == 0U ||
        g_window_server.composite_framebuffer == 0 ||
        g_window_server.composite_framebuffer ==
            g_window_server.framebuffer ||
        snapshot->window_count == 0U ||
        snapshot->windows[snapshot->window_count - 1U].identifier !=
            snapshot->dragging_identifier) {
        return;
    }

    local_left = 0;
    local_top = 0;
    local_right = snapshot->drag_width;
    local_bottom = snapshot->drag_height;

    if (snapshot->drag_old_x < 0 &&
        local_left < -(int64_t)snapshot->drag_old_x) {
        local_left = -(int64_t)snapshot->drag_old_x;
    }
    if (snapshot->drag_new_x < 0 &&
        local_left < -(int64_t)snapshot->drag_new_x) {
        local_left = -(int64_t)snapshot->drag_new_x;
    }
    if (snapshot->drag_old_y < 0 &&
        local_top < -(int64_t)snapshot->drag_old_y) {
        local_top = -(int64_t)snapshot->drag_old_y;
    }
    if (snapshot->drag_new_y < 0 &&
        local_top < -(int64_t)snapshot->drag_new_y) {
        local_top = -(int64_t)snapshot->drag_new_y;
    }

    {
        int64_t old_visible_right =
            (int64_t)g_window_server.display_width -
            snapshot->drag_old_x;
        int64_t new_visible_right =
            (int64_t)g_window_server.display_width -
            snapshot->drag_new_x;
        int64_t old_visible_bottom =
            (int64_t)g_window_server.display_height -
            snapshot->drag_old_y;
        int64_t new_visible_bottom =
            (int64_t)g_window_server.display_height -
            snapshot->drag_new_y;

        if (local_right > old_visible_right) {
            local_right = old_visible_right;
        }
        if (local_right > new_visible_right) {
            local_right = new_visible_right;
        }
        if (local_bottom > old_visible_bottom) {
            local_bottom = old_visible_bottom;
        }
        if (local_bottom > new_visible_bottom) {
            local_bottom = new_visible_bottom;
        }
    }

    if (local_left < 0) local_left = 0;
    if (local_top < 0) local_top = 0;

    if (local_left >= local_right ||
        local_top >= local_bottom) {
        return;
    }

    width = (uint32_t)(local_right - local_left);
    height = (uint32_t)(local_bottom - local_top);

    for (uint32_t row_index = 0U;
         row_index < height;
         ++row_index) {
        uint32_t row =
            snapshot->drag_new_y > snapshot->drag_old_y ?
                height - 1U - row_index :
                row_index;

        int64_t local_y = local_top + (int64_t)row;
        int64_t source_y =
            (int64_t)snapshot->drag_old_y + local_y;
        int64_t destination_y =
            (int64_t)snapshot->drag_new_y + local_y;

        int64_t source_x =
            (int64_t)snapshot->drag_old_x + local_left;
        int64_t destination_x =
            (int64_t)snapshot->drag_new_x + local_left;

        volatile uint32_t *source =
            g_window_server.composite_framebuffer +
            (uint64_t)source_y * g_window_server.display_stride +
            (uint32_t)source_x;

        volatile uint32_t *destination =
            g_window_server.composite_framebuffer +
            (uint64_t)destination_y * g_window_server.display_stride +
            (uint32_t)destination_x;

        if (source_y == destination_y &&
            destination_x > source_x) {
            for (uint32_t column = width;
                 column != 0U;
                 --column) {
                destination[column - 1U] =
                    source[column - 1U];
            }
        } else {
            for (uint32_t column = 0U;
                 column < width;
                 ++column) {
                destination[column] =
                    source[column];
            }
        }
    }
}

'''
    return replace_between(text, start, end, replacement, "retained drag blit")

def patch_drag_snapshot_and_finish(text):
    old = '''    compositor_collapse_drag_damage(snapshot);
'''
    new = '''    if (!snapshot->drag_blit_valid) {
        compositor_collapse_drag_damage(snapshot);
    }
'''
    text = replace_once(text, old, new, "drag damage collapse")

    old = '''    g_window_server.composing = false;

    window_unlock();
'''
    new = '''    if (g_window_server.dragging_identifier != 0U &&
        g_window_server.resize_edges == 0U) {
        window_server_window_t *dragged =
            find_window_locked(g_window_server.dragging_identifier);

        g_window_server.drag_blit_valid =
            window_drag_reuse_available_locked(dragged);
    } else {
        g_window_server.drag_blit_valid = false;
    }

    g_window_server.composing = false;

    window_unlock();
'''
    text = replace_once(text, old, new, "arm retained drag after frame")
    return text

def patch_drag_motion(text):
    old = '''        g_window_server.drag_blit_valid = false;

        /*
         * One old/new damage pair for the complete diagonal move.
         */
        if (old_x != new_x ||
            old_y != new_y) {
'''
    new = '''        bool reuse_retained =
            g_window_server.drag_blit_valid &&
            window_drag_reuse_available_locked(dragged);

        /*
         * One old/new damage pair for the complete diagonal move.
         */
        if (old_x != new_x ||
            old_y != new_y) {
'''
    text = replace_once(text, old, new, "batch retained eligibility")

    old = '''            /* A captured window is normally raised by focus_locked() on
             * press.  Only a genuinely topmost surface may reuse composite
             * pixels; otherwise a window above it could be copied away. */
            /* Keep the retained-pixel experiment disabled until the source
             * frame can be proven free of cursor/underlay pixels.  A drag
             * must prefer a complete, correct composition over a possible
             * stale-pixel shortcut. */
            g_window_server.drag_blit_valid = false;
            g_window_server.drag_old_x = old_x;
'''
    new = '''            g_window_server.drag_blit_valid =
                reuse_retained;
            g_window_server.drag_old_x = old_x;
'''
    text = replace_once(text, old, new, "enable batch retained move")

    old = '''            window_mark_moved_rect_locked(
                old_x,
                old_y,
                new_x,
                new_y,
                outer_width,
                outer_height,
                !g_window_server.drag_blit_valid);

            dragged->dirty =
                true;
'''
    new = '''            window_mark_moved_rect_locked(
                old_x,
                old_y,
                new_x,
                new_y,
                outer_width,
                outer_height,
                !g_window_server.drag_blit_valid);

            if (g_window_server.drag_blit_valid) {
                window_mark_drag_corner_repair_locked(
                    new_x,
                    new_y,
                    outer_width,
                    outer_height,
                    dragged->flags);
            }

            dragged->dirty =
                true;
'''
    text = replace_once(text, old, new, "batch drag corner repair")

    old = '''                int32_t old_x = dragged->x;
                int32_t old_y = dragged->y;
                dragged->x = (int32_t)g_window_server.pointer_x -
                             g_window_server.drag_offset_x;
                dragged->y = (int32_t)g_window_server.pointer_y -
                             g_window_server.drag_offset_y;
                window_mark_moved_rect_locked(
                    old_x, old_y, dragged->x, dragged->y,
                    window_outer_width(dragged->width, dragged->flags),
                    window_outer_height(dragged->height, dragged->flags),
                    true);
                dragged->dirty = true;
'''
    new = '''                int32_t old_x = dragged->x;
                int32_t old_y = dragged->y;
                int32_t new_x =
                    (int32_t)g_window_server.pointer_x -
                    g_window_server.drag_offset_x;
                int32_t new_y =
                    (int32_t)g_window_server.pointer_y -
                    g_window_server.drag_offset_y;
                uint32_t outer_width =
                    window_outer_width(dragged->width, dragged->flags);
                uint32_t outer_height =
                    window_outer_height(dragged->height, dragged->flags);
                bool reuse_retained =
                    g_window_server.drag_blit_valid &&
                    window_drag_reuse_available_locked(dragged);

                g_window_server.drag_blit_valid = reuse_retained;
                g_window_server.drag_old_x = old_x;
                g_window_server.drag_old_y = old_y;
                g_window_server.drag_new_x = new_x;
                g_window_server.drag_new_y = new_y;
                g_window_server.drag_width = outer_width;
                g_window_server.drag_height = outer_height;

                dragged->x = new_x;
                dragged->y = new_y;

                window_mark_moved_rect_locked(
                    old_x, old_y, new_x, new_y,
                    outer_width, outer_height,
                    !reuse_retained);

                if (reuse_retained) {
                    window_mark_drag_corner_repair_locked(
                        new_x, new_y,
                        outer_width, outer_height,
                        dragged->flags);
                }

                dragged->dirty = true;
'''
    text = replace_once(text, old, new, "legacy retained drag")
    return text

def patch_render_cursor_publish(text):
    old = '''    } else {
        compositor_commit_snapshot();
    }
}
'''
    new = '''    } else {
        compositor_commit_snapshot();
        compositor_present_cursor_direct(true);
    }
}
'''
    return replace_once(text, old, new, "post-frame cursor overlay")

def patch_parallel_copy(text):
    old = '''    if (pixels <= WINDOW_COMPOSITOR_ATOMIC_PIXELS) return false;
'''
    new = '''    if (pixels <= WINDOW_COMPOSITOR_PARALLEL_PIXELS) return false;
'''
    text = replace_once(text, old, new, "parallel threshold")

    old = '''        if (candidate == compositor_cpu ||
            !x86_smp_cpu_online(candidate)) {
            continue;
        }
'''
    new = '''        if (candidate == compositor_cpu ||
            candidate == current_cpu ||
            !x86_smp_cpu_online(candidate)) {
            continue;
        }
'''
    text = replace_once(text, old, new, "copy worker cpu reservation")

    old = '''        if (pixels > WINDOW_COMPOSITOR_ATOMIC_PIXELS &&
            compositor_copy_rect_parallel_buffers(
                g_window_server.composite_framebuffer,
                (const volatile uint32_t *)g_desktop_cache,
                g_window_server.display_stride,
                g_desktop_cache_stride,
                &cached_rect)) {
            return;
        }
'''
    new = '''        if (WINDOW_COMPOSITOR_PARALLEL_WB_COPY != 0U &&
            pixels > WINDOW_COMPOSITOR_PARALLEL_PIXELS &&
            compositor_copy_rect_parallel_buffers(
                g_window_server.composite_framebuffer,
                (const volatile uint32_t *)g_desktop_cache,
                g_window_server.display_stride,
                g_desktop_cache_stride,
                &cached_rect)) {
            return;
        }
'''
    text = replace_once(text, old, new, "WB desktop restore")

    old = '''    if (commit_as_move_transaction &&
        g_compositor_snapshot.damage_count == 1U &&
        compositor_copy_rect_parallel(
            &g_compositor_snapshot.damage_rects[0])) {
        return;
    }
'''
    new = '''    if (commit_as_move_transaction) {
        uint32_t left = g_window_server.display_width;
        uint32_t top = g_window_server.display_height;
        uint32_t right = 0U;
        uint32_t bottom = 0U;

        for (uint32_t region = 0U;
             region < g_compositor_snapshot.damage_count;
             ++region) {
            const window_damage_rect_t *rect =
                &g_compositor_snapshot.damage_rects[region];

            if (rect->left < left) left = rect->left;
            if (rect->top < top) top = rect->top;
            if (rect->right > right) right = rect->right;
            if (rect->bottom > bottom) bottom = rect->bottom;
        }

        if (left < right && top < bottom) {
            window_damage_rect_t transaction = {
                .left = left,
                .top = top,
                .right = right,
                .bottom = bottom,
            };

            if (compositor_copy_rect_parallel(&transaction)) {
                return;
            }
        }
    }
'''
    text = replace_once(text, old, new, "parallel drag scanout")
    return text

def apply_v2(text):
    if V2_MARKER in text:
        return text

    text = patch_constants(text)
    text = patch_state(text)
    text = patch_worker_immediate_cursor(text)
    text = patch_drag_helpers(text)
    text = patch_cursor_damage(text)
    text = patch_cursor_scene_and_direct_publish(text)
    text = patch_drag_blit_function(text)
    text = patch_drag_snapshot_and_finish(text)
    text = patch_drag_motion(text)
    text = patch_render_cursor_publish(text)
    text = patch_parallel_copy(text)
    return text

def rollback(repo):
    backup = repo / f".compositor-v2-backup-{EXPECTED_HEAD[:8]}" / TARGET
    if not backup.exists():
        die(f"v2 backup not found: {backup}")
    shutil.copy2(backup, repo / TARGET)
    print(f"Restored {TARGET} from {backup}")

def main():
    parser = argparse.ArgumentParser(
        description="Apply LiteOS retained-drag compositor optimization v2")
    parser.add_argument("repo", nargs="?", default=".")
    parser.add_argument("--rollback", action="store_true")
    parser.add_argument("--force-head", action="store_true")
    parser.add_argument("--force-dirty", action="store_true")
    parser.add_argument("--check-only", action="store_true")
    args = parser.parse_args()

    repo = Path(args.repo).resolve()
    if not (repo / ".git").exists():
        die(f"not a git repository: {repo}")

    if args.rollback:
        rollback(repo)
        return

    head = git_head(repo)
    if head != EXPECTED_HEAD and not args.force_head:
        die(
            f"HEAD is {head}, expected {EXPECTED_HEAD}. "
            "Regenerate/review v2 for the new baseline."
        )

    target = repo / TARGET
    if not target.exists():
        die(f"missing {TARGET}")

    text = target.read_text(encoding="utf-8")

    if V2_MARKER in text:
        print("v2 is already applied.")
        return

    text, restored_v1 = restore_v1_if_needed(repo, text)

    if not restored_v1:
        dirty = git(repo, "diff", "--quiet", "HEAD", "--", str(TARGET),
                    check=False).returncode != 0
        if dirty and not args.force_dirty:
            die(
                f"{TARGET} has local edits not recognized as v1. "
                "Use --force-dirty only after reviewing them."
            )

    patched = apply_v2(text)

    required = [
        V2_MARKER,
        "compositor_present_cursor_direct",
        "window_drag_reuse_available_locked",
        "window_mark_drag_corner_repair_locked",
        "WINDOW_COMPOSITOR_PARALLEL_PIXELS",
    ]
    for needle in required:
        if needle not in patched:
            die(f"internal validation failed: missing {needle}")

    if args.check_only:
        print("PASS: all v2 source anchors matched.")
        print("No file was modified.")
        return

    backup = backup_v2(repo)
    target.write_text(patched, encoding="utf-8", newline="\n")

    print(f"Applied {V2_MARKER}")
    print(f"Baseline: {EXPECTED_HEAD}")
    print(f"Backup:   {backup}")
    print(f"Changed:  {TARGET}")
    print("")
    print("Next:")
    print("  git diff -- kernel/graphics/window_server.c")
    print("  make clean")
    print("  make -j$(nproc)")
    print("  make test")
    print("")
    print("Runtime checks:")
    print("  - cursor should remain smooth even when scene FPS is 60")
    print("  - first drag frame may be full; later frames reuse retained pixels")
    print("  - no cursor trails")
    print("  - no rounded-corner stale pixels")

if __name__ == "__main__":
    main()
