#include <kernel/sched.h>
#include "internal.h"

/* REFACTOR_P7A_PUBLICATION_OWNER: damage and scanout publication policy. */

uint64_t compositor_publication_damage_pixels(void) {
    uint64_t pixels = 0U;

    for (uint32_t region = 0U;
         region < g_compositor_snapshot.damage_count;
         ++region) {
        const Rect *rect =
            &g_compositor_snapshot.damage_rects[region];
        uint32_t width;
        uint32_t height;
        uint64_t area;

        if (rect->x0 >= rect->x1 || rect->y0 >= rect->y1) continue;
        width = rect->x1 - rect->x0;
        height = rect->y1 - rect->y0;
        area = (uint64_t)width * height;
        if (UINT64_MAX - pixels < area) return UINT64_MAX;
        pixels += area;
    }
    return pixels;
}

/* Publish one damage rectangle.  The caller holds one preemption disable
 * around the publication.  Large ordinary updates periodically drop that
 * disable and yield so an incoming HID report can run; drag/move frames keep
 * the single atomic transaction by passing allow_yield=false. */
void compositor_publication_rect(const Rect *rect,
                                    bool allow_yield) {
    Rect clipped;
    uint32_t width;
    uint64_t pixels_since_yield = 0U;

    if (rect == 0 || g_window_server.framebuffer == 0 ||
        g_window_server.composite_framebuffer == 0 ||
        g_window_server.display_stride < g_window_server.display_width) {
        return;
    }
    /* Final publication owns clipping even when an upstream damage producer
     * retained coordinates from an older geometry. Clip before converting
     * signed positions to scanline indices or subtracting widths. */
    if (!rect_clip_to_extent(rect->x0, rect->y0, rect->x1, rect->y1,
            g_window_server.display_width, g_window_server.display_height,
            &clipped)) return;
    rect = &clipped;

    width =
        rect->x1 -
        rect->x0;

    for (uint32_t row = (uint32_t)rect->y0;
         row < (uint32_t)rect->y1;
         ++row) {
        volatile uint32_t *destination =
            g_window_server.framebuffer +
            (uint64_t)row * g_window_server.display_stride +
            rect->x0;

        /*
         * composite_framebuffer is guaranteed to be the private retained WB
         * scene on this path; the direct-framebuffer fallback never calls
         * compositor_commit_snapshot().
         */
        const uint32_t *source =
            (const uint32_t *)(uintptr_t)(
                g_window_server.composite_framebuffer +
                (uint64_t)row * g_window_server.display_stride +
                rect->x0);

        if (!window_present_cursor_publish_span(
                destination,
                source,
                rect->x0,
                row,
                width)) {
            compositor_copy_wc_scanline(
                destination,
                source,
                width);
        }

        if (!allow_yield) {
            continue;
        }

        pixels_since_yield +=
            width;

        if (pixels_since_yield <
                WINDOW_COMPOSITOR_PUBLICATION_YIELD_PIXELS ||
            row + 1U >= (uint32_t)rect->y1) {
            continue;
        }

        /*
         * Yield by copied work, not by row count.  A narrow 64-pixel damage
         * rectangle no longer calls schedule() every 16 rows while a full
         * 2560-pixel row gets a proportionally shorter quantum.
         *
         * Flush this CPU's WC stores before a scheduling point: the worker
         * can resume on a different CPU. Each scanline already preserves the
         * visible cursor, including on QEMU's fixed page. The immutable scene
         * and single compositor owner remain held across the scheduling point.
         */
        __asm__ volatile ("sfence" : : : "memory");
        sched_preempt_enable();
        schedule();
        sched_preempt_disable();

        pixels_since_yield = 0U;
    }
}


/*
 * Clip and publish one rectangle from the completed retained WB scene.
 *
 * This helper is used only by the GOP drag ghost-suppression prepass below.
 * The normal transaction still executes afterward and remains the authority
 * for the final visible frame.
 */
static void compositor_publication_drag_exposure_rect(
    int64_t left,
    int64_t top,
    int64_t right,
    int64_t bottom) {

    Rect rect;

    if (!rect_clip_to_extent(left, top, right, bottom,
            g_window_server.display_width, g_window_server.display_height,
            &rect)) return;

    compositor_publication_rect(
        &rect,
        false);
}


/*
 * GOP has no page flip: the display engine can scan the visible framebuffer
 * while the CPU is copying a large moved-window transaction into it.
 *
 * The visually worst artifact is the trailing OLD\NEW edge remaining visible
 * while the multi-megabyte transaction is still progressing. The retained WB
 * scene already contains the final background/lower-window pixels there.
 *
 * Publish only those old-position exposure strips first. A one-pixel move of
 * an 800px-high window adds only ~3.2 KiB of WC traffic but removes the old
 * silhouette edge before the full transaction starts.
 *
 * This does not make GOP atomic; native scanout/page flip remains the complete
 * solution. It is a low-cost correctness-oriented mitigation for GOP.
 */
void compositor_publication_drag_old_exposure(void) {
    const compositor_snapshot_t *snapshot =
        &g_compositor_snapshot;

    int64_t old_left;
    int64_t old_top;
    int64_t old_right;
    int64_t old_bottom;

    int64_t new_left;
    int64_t new_top;
    int64_t new_right;
    int64_t new_bottom;

    int64_t overlap_left;
    int64_t overlap_top;
    int64_t overlap_right;
    int64_t overlap_bottom;

    if (!snapshot->drag_blit_valid ||
        snapshot->drag_width == 0U ||
        snapshot->drag_height == 0U) {
        return;
    }

    old_left = snapshot->drag_old_x;
    old_top = snapshot->drag_old_y;
    old_right =
        old_left + (int64_t)snapshot->drag_width;
    old_bottom =
        old_top + (int64_t)snapshot->drag_height;

    new_left = snapshot->drag_new_x;
    new_top = snapshot->drag_new_y;
    new_right =
        new_left + (int64_t)snapshot->drag_width;
    new_bottom =
        new_top + (int64_t)snapshot->drag_height;

    overlap_left =
        old_left > new_left ?
            old_left :
            new_left;

    overlap_top =
        old_top > new_top ?
            old_top :
            new_top;

    overlap_right =
        old_right < new_right ?
            old_right :
            new_right;

    overlap_bottom =
        old_bottom < new_bottom ?
            old_bottom :
            new_bottom;

    if (overlap_left >= overlap_right ||
        overlap_top >= overlap_bottom) {

        compositor_publication_drag_exposure_rect(
            old_left,
            old_top,
            old_right,
            old_bottom);

        return;
    }

    /* OLD \ NEW horizontal strip, full old height. */
    if (new_left > old_left) {
        compositor_publication_drag_exposure_rect(
            old_left,
            old_top,
            overlap_left,
            old_bottom);
    } else if (new_left < old_left) {
        compositor_publication_drag_exposure_rect(
            overlap_right,
            old_top,
            old_right,
            old_bottom);
    }

    /*
     * OLD \ NEW vertical strip over only the horizontal overlap, so it never
     * double-publishes the horizontal strip above.
     */
    if (new_top > old_top) {
        compositor_publication_drag_exposure_rect(
            overlap_left,
            old_top,
            overlap_right,
            overlap_top);
    } else if (new_top < old_top) {
        compositor_publication_drag_exposure_rect(
            overlap_left,
            overlap_bottom,
            overlap_right,
            old_bottom);
    }
}

