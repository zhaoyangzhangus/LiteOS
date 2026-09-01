#include <kernel/sched.h>
#include <kernel/qemu_stdvga.h>
#include "internal.h"

/* REFACTOR_P7A_COMPOSITOR_OWNER: frame orchestration. */
/* Linux Xcursor/Adwaita top_left_arrow, 24x24, hotspot (3,1).
 * The source is an ARGB cursor surface; it is composited pixel by pixel
 * with the same source-over rule as a Linux software cursor. */








/*
 * Return the screen-space rectangle of one titlebar button.
 *
 * slot from right:
 *
 *     close      = 0
 *     maximize   = 1
 *     minimize   = 2
 */




void compositor_commit_snapshot(void) {
    bool commit_as_move_transaction;
    bool allow_yield;

    if (g_window_server.framebuffer == 0 ||
        g_window_server.composite_framebuffer == 0 ||
        g_window_server.composite_framebuffer ==
            g_window_server.framebuffer ||
        g_compositor_snapshot.damage_count == 0U) {
        return;
    }

    /*
     * Native hidden-VRAM publication replaces the GOP visible copy entirely.
     * The final Y_OFFSET write is the only operation that exposes this frame.
     */
    if (qemu_stdvga_flip_available() &&
        compositor_commit_qemu_stdvga()) {
        compositor_present_mark_scanout_flipped();
        return;
    }

    commit_as_move_transaction =
        g_compositor_snapshot.dragging_identifier != 0U;

    /* Keep cursor-sized and other tiny updates atomic (there is no useful
     * scheduling point in a 24x24 copy), but make a large ordinary client
     * repaint preemptible in bounded row chunks.  A drag/move is always kept
     * atomic until a real page-flip path exists. */
    allow_yield = !commit_as_move_transaction &&
                  compositor_publication_damage_pixels() >
                  WINDOW_COMPOSITOR_ATOMIC_PIXELS;

    /*
     * Large ordinary single-rectangle publication.
     *
     * This is deliberately evaluated before sched_preempt_disable(): the
     * persistent helper path may sleep/yield while waiting for remote CPUs and
     * therefore requires preemption to remain enabled.
     *
     * Keep fragmented damage on the serial path. Waking helpers once per tiny
     * rectangle would destroy the benefit of the spatial damage system.
     */
    if (WINDOW_COMPOSITOR_PARALLEL_ORDINARY != 0U &&
        !commit_as_move_transaction &&
        g_compositor_snapshot.damage_count == 1U) {

        const Rect *ordinary =
            &g_compositor_snapshot.damage_rects[0];

        if (ordinary->x0 < ordinary->x1 &&
            ordinary->y0 < ordinary->y1) {

            uint64_t ordinary_pixels =
                (uint64_t)(ordinary->x1 - ordinary->x0) *
                (uint64_t)(ordinary->y1 - ordinary->y0);

            if (ordinary_pixels >
                    WINDOW_COMPOSITOR_PARALLEL_ORDINARY_PIXELS &&
                compositor_copy_rect_parallel(ordinary)) {
                return;
            }
        }
    }

    /* Drag/resize frames are already collapsed to one complete rectangle.
     * Copy that transaction on disjoint rows across helper CPUs.  The helper
     * path includes per-CPU fences and returns only after every row is visible,
     * so it cannot expose the stale-gap corruption caused by the old bounding
     * box copy. */
    if (WINDOW_COMPOSITOR_PARALLEL_DRAG != 0U &&
        commit_as_move_transaction) {
        uint32_t left = g_window_server.display_width;
        uint32_t top = g_window_server.display_height;
        uint32_t right = 0U;
        uint32_t bottom = 0U;

        for (uint32_t region = 0U;
             region < g_compositor_snapshot.damage_count;
             ++region) {
            const Rect *rect =
                &g_compositor_snapshot.damage_rects[region];

            if ((uint32_t)rect->x0 < left) left = (uint32_t)rect->x0;
            if ((uint32_t)rect->y0 < top) top = (uint32_t)rect->y0;
            if ((uint32_t)rect->x1 > right) right = (uint32_t)rect->x1;
            if ((uint32_t)rect->y1 > bottom) bottom = (uint32_t)rect->y1;
        }

        if (left < right && top < bottom) {
            Rect transaction = {
                .x0 = left,
                .y0 = top,
                .x1 = right,
                .y1 = bottom,
            };

            if (compositor_copy_rect_parallel(&transaction)) {
                return;
            }
        }
    }

    /*
     * Only GOP publication is non-preemptible.  All expensive scene
     * composition has already completed in normal WB memory.
     */
    sched_preempt_disable();

    if (commit_as_move_transaction) {
        /*
         * Erase the retained drag's old-only trailing edge before starting the
         * large visible bounding transaction. The extra traffic is normally
         * only delta_x*height + delta_y*overlap_width pixels.
         */
        compositor_publication_drag_old_exposure();

        uint32_t left =
            g_window_server.display_width;

        uint32_t top =
            g_window_server.display_height;

        uint32_t right = 0U;
        uint32_t bottom = 0U;

        /*
         * A window move usually contributes:
         *
         *   old window
         *   new window
         *   old cursor
         *   new cursor
         *
         * They were rendered independently into the backbuffer for
         * efficiency, but publishing them independently lets scanout observe
         * the old area after it has been erased and before the new titlebar
         * has arrived.  Publish their bounding transaction instead.
         */
        for (uint32_t region = 0U;
             region < g_compositor_snapshot.damage_count;
             ++region) {

            const Rect *rect =
                &g_compositor_snapshot.damage_rects[region];

            if ((uint32_t)rect->x0 < left) {
                left = (uint32_t)rect->x0;
            }

            if ((uint32_t)rect->y0 < top) {
                top = (uint32_t)rect->y0;
            }

            if ((uint32_t)rect->x1 > right) {
                right = (uint32_t)rect->x1;
            }

            if ((uint32_t)rect->y1 > bottom) {
                bottom = (uint32_t)rect->y1;
            }
        }

        if (left < right && top < bottom) {
            Rect transaction = {
                .x0 = left,
                .y0 = top,
                .x1 = right,
                .y1 = bottom,
            };
            compositor_publication_rect(&transaction, false);
        }
    } else {
        /*
         * Ordinary unrelated damages remain separate.  This preserves the
         * small-damage optimization for cursor movement, app updates, etc.
         */
        for (uint32_t region = 0U;
             region < g_compositor_snapshot.damage_count;
             ++region) {

            const Rect *rect =
                &g_compositor_snapshot.damage_rects[region];

            uint32_t left = rect->x0;
            uint32_t top = rect->y0;
            uint32_t right = rect->x1;
            uint32_t bottom = rect->y1;

            if (left >= right || top >= bottom) {
                continue;
            }

            Rect ordinary = {
                .x0 = left,
                .y0 = top,
                .x1 = right,
                .y1 = bottom,
            };
            compositor_publication_rect(&ordinary, allow_yield);
        }
    }

    /*
     * Complete all WC stores before scanout may observe the transaction.
     */
    __asm__ volatile (
        "sfence"
        :
        :
        : "memory");

    sched_preempt_enable();
}


/*
 * Return true only when this scene publication can overwrite pixels belonging
 * to the software cursor that is currently visible on scanout.
 *
 * If the pointer moves while/after composition, compositor_present_cursor_direct
 * already notices the coordinate change even with force=false.  Therefore the
 * force path is needed only when scene damage intersects the old presented
 * cursor rectangle.
 */
/*
 * Device-memory publication belongs to display core.
 *
 * P5 still owns damage selection, row partitioning and transaction policy;
 * this leaf call only performs the architecture/output-specific WB -> scanout
 * span write.  That keeps the compositor independent from MOVDIR64B/MOVNTI
 * details and gives DISPLAY_COMMIT the same optimized GOP path.
 */
const uint32_t g_linux_cursor_argb[WINDOW_CURSOR_WIDTH * WINDOW_CURSOR_HEIGHT] = {
    0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x00000000U, 0x02000000U, 0x45414141U, 0x03000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x01000000U, 0x09000000U, 0xF7F5F5F5U, 0x4B414141U, 0x04000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x02000000U, 0x15000000U, 0xFFFFFFFFU, 0xF7EAEAEAU, 0x4E414141U, 0x04000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x03000000U, 0x1A000000U, 0xFFFFFFFFU, 0xFF3C3C3CU, 0xF7E9E9E9U, 0x50434343U, 0x04000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x03000000U, 0x1B000000U, 0xFFFFFFFFU, 0xFF000000U, 0xFF3C3C3CU, 0xF7E9E9E9U, 0x50434343U, 0x04000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x03000000U, 0x1B000000U, 0xFFFFFFFFU, 0xFF000000U, 0xFF000000U, 0xFF3C3C3CU, 0xF7E9E9E9U, 0x50434343U, 0x04000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x03000000U, 0x1B000000U, 0xFFFFFFFFU, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF3C3C3CU, 0xF7E9E9E9U, 0x50434343U, 0x04000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x03000000U, 0x1B000000U, 0xFFFFFFFFU, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF3C3C3CU, 0xF7E9E9E9U, 0x50434343U, 0x04000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x03000000U, 0x1B000000U, 0xFFFFFFFFU, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF3C3C3CU, 0xF7E9E9E9U, 0x50434343U, 0x04000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x03000000U, 0x1B000000U, 0xFFFFFFFFU, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF3C3C3CU, 0xF7E9E9E9U, 0x50434343U, 0x04000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x03000000U, 0x1B000000U, 0xFFFFFFFFU, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF3C3C3CU, 0xF7E9E9E9U, 0x50434343U, 0x04000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x03000000U, 0x1B000000U, 0xFFFFFFFFU, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF3C3C3CU, 0xF7E9E9E9U, 0x50434343U, 0x04000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x03000000U, 0x1B000000U, 0xFFFFFFFFU, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFDC1C1C1U, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xF8F5F5F5U, 0x4C414141U, 0x03000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x03000000U, 0x1B000000U, 0xFFFFFFFFU, 0xFF000000U, 0xFF000000U, 0xFF414141U, 0xFF404040U, 0xFF000000U, 0xFF4D4D4DU, 0xE8CBCBCBU, 0x5F000000U, 0x59000000U, 0x54000000U, 0x3F000000U, 0x1A000000U, 0x05000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x03000000U, 0x1B000000U, 0xFFFFFFFFU, 0xFF000000U, 0xFF414141U, 0xFCF3F3F3U, 0xF7BABABAU, 0xFF000000U, 0xFF010101U, 0xF7D6D6D6U, 0x65414141U, 0x1E000000U, 0x1A000000U, 0x15000000U, 0x09000000U, 0x03000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x03000000U, 0x1B000000U, 0xFFFFFFFFU, 0xFF3D3D3DU, 0xF9E9E9E9U, 0x90464646U, 0xF4E1E1E1U, 0xFF323232U, 0xFF000000U, 0xFE626262U, 0xCDB5B5B5U, 0x0E000000U, 0x04000000U, 0x02000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x03000000U, 0x1B000000U, 0xFFFFFFFFU, 0xF9EAEAEAU, 0x82404040U, 0x43000000U, 0xA5737373U, 0xF9A5A5A5U, 0xFF000000U, 0xFF070707U, 0xF8E5E5E5U, 0x3E2C2C2CU, 0x03000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x03000000U, 0x1A000000U, 0xF9F4F4F4U, 0x7F404040U, 0x30000000U, 0x1E000000U, 0x460E0E0EU, 0xF8EBEBEBU, 0xFF1F1F1FU, 0xFF000000U, 0xFD787878U, 0xB79F9F9FU, 0x09000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x02000000U, 0x15000000U, 0x6F404040U, 0x2D000000U, 0x11000000U, 0x0A000000U, 0x21000000U, 0xB4898989U, 0xFB8E8E8EU, 0xFF000000U, 0xFF101010U, 0xF9F0F0F0U, 0x19070707U, 0x02000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x01000000U, 0x09000000U, 0x19000000U, 0x0D000000U, 0x04000000U, 0x03000000U, 0x11000000U, 0x521A1A1AU, 0xF9ECECECU, 0xFF1F1F1FU, 0xFF202020U, 0xFAF0F0F0U, 0x1E060606U, 0x03000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x00000000U, 0x02000000U, 0x05000000U, 0x03000000U, 0x01000000U, 0x01000000U, 0x07000000U, 0x23000000U, 0x99636363U, 0xFAF0F0F0U, 0xFAF0F0F0U, 0x96626262U, 0x17000000U, 0x02000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x02000000U, 0x0F000000U, 0x31000000U, 0x53050505U, 0x52050505U, 0x31000000U, 0x0E000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x01000000U, 0x04000000U, 0x0E000000U, 0x17000000U, 0x17000000U, 0x0E000000U, 0x04000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U
};

/* Cursor rasterization is owned by cursor_occlusion.c.
     * Clip once:
     *
     *     cursor rectangle
     *       ∩ screen
     *       ∩ current damage
     *
     * The old implementation visited all 24x24 source pixels and repeated
     * these bounds checks inside compositor_blend_pixel_locked().
     */
compositor_snapshot_t g_compositor_snapshot;

/* REFACTOR_P7B_SNAPSHOT_TEST_OWNER: test fixtures must not write snapshots. */
void compositor_snapshot_test_clear_damage_tiles(void) {
    for (uint32_t word = 0U;
         word < WINDOW_DAMAGE_TILE_WORDS;
         ++word) {
        g_compositor_snapshot.damage_tiles[word] = 0U;
    }
}

bool compositor_snapshot_test_set_damage_tile(uint32_t tile_index) {
    uint32_t word;
    uint32_t bit;

    if (tile_index >= WINDOW_DAMAGE_TILE_WORDS * 64U) {
        return false;
    }

    word = tile_index / 64U;
    bit = tile_index % 64U;
    g_compositor_snapshot.damage_tiles[word] |=
        1ULL << bit;
    return true;
}

void compositor_reset_state_locked(void) {
    g_window_server.composing = false;
}

/* REFACTOR_P7B_FRAME_OWNER: owns the snapshot -> plan -> render -> finish order. */
void compositor_frame_run(void) {
    bool compose;

    /* Only the immutable scene capture runs under the window lock. */
    window_lock();
    compose = compositor_snapshot_begin_locked();
    window_unlock();

    if (!compose) return;
    compositor_snapshot_plan();
    compositor_render_snapshot();
    compositor_snapshot_finish();
}

/*
 * P16 capture phase.
 *
 * Called with window_lock held. Expensive planning is forbidden here.
 */
bool compositor_snapshot_begin_locked(void) {
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
        window_damage_clear_pending_locked();
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
    /*
     * This generation now belongs exclusively to the immutable snapshot.
     * Any producer after unlock sees clean state and publishes a new edge.
     */
    window_damage_rotate_locked();

    return true;
}


/*
 * P16 planning phase. Called without window_lock.
 */
void compositor_snapshot_plan(void) {
    compositor_snapshot_t *snapshot =
        &g_compositor_snapshot;

    uint32_t original_window_count;
    uint32_t first_window_index;
    uint32_t write_index = 0U;

    if (snapshot->damage_full_captured) {
        snapshot->damage_count = 1U;

        snapshot->damage_rects[0] =
            (Rect){
                .x0 = 0U,
                .y0 = 0U,
                .x1 =
                    g_window_server.display_width,
                .y1 =
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
            (Rect){
                .x0 = 0U,
                .y0 = 0U,
                .x1 =
                    g_window_server.display_width,
                .y1 =
                    g_window_server.display_height,
            };
    }

    if (!snapshot->drag_blit_valid) {
        compositor_collapse_drag_damage(
            snapshot);
    }

    /* P7B: materialize touch/full/opaque masks before render planning. */
    (void)compositor_tile_metadata_build(snapshot);

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

    /*
     * Adaptive: the builder immediately returns for simple frames.
     */
    compositor_build_occlusion_floor_cache(
        snapshot);

    /*
     * P19 consumes the P18 floor cache and materializes the final spatial
     * render program.  Failure merely selects the existing P18 path.
     */
    (void)compositor_build_render_plan(
        snapshot);

    snapshot->damage_full_captured =
        false;

    snapshot->damage_tiles_captured =
        false;
}

void compositor_render_snapshot(void) {
    bool direct_framebuffer =
        g_window_server.composite_framebuffer ==
        g_window_server.framebuffer;

    /*
     * Normal path:
     *     compose framebuffer (RAM) -> preemptible
     *     GOP publication          -> one short non-preemptible transaction
     *
     * Low-memory fallback has no private compose buffer, so preserve atomic
     * rendering there rather than expose intermediate drawing operations.
     */
    if (direct_framebuffer) {
        sched_preempt_disable();
    }

    if (!direct_framebuffer) {
        compositor_blit_drag_overlap();
    }

    if (g_compositor_render_plan_valid) {
        /*
         * P19 hot path: occlusion/planning is already complete.
         */
        for (uint32_t index = 0U;
             index < g_compositor_render_plan_count;
             ++index) {

            const compositor_render_span_t *span =
                &g_compositor_render_plan[index];

            g_compositor_snapshot.damage_bounds.x0 =
                span->rect.x0;

            g_compositor_snapshot.damage_bounds.y0 =
                span->rect.y0;

            g_compositor_snapshot.damage_bounds.x1 =
                span->rect.x1;

            g_compositor_snapshot.damage_bounds.y1 =
                span->rect.y1;

            compositor_region_locked(
                span);
        }
    } else {
        /*
         * Simple frame or pathological plan overflow: retain P18 exactly.
         */
        for (uint32_t index = 0U;
             index < g_compositor_snapshot.damage_count;
             ++index) {

            const Rect *rect =
                &g_compositor_snapshot.damage_rects[index];

            g_compositor_snapshot.damage_bounds.x0 =
                rect->x0;

            g_compositor_snapshot.damage_bounds.y0 =
                rect->y0;

            g_compositor_snapshot.damage_bounds.x1 =
                rect->x1;

            g_compositor_snapshot.damage_bounds.y1 =
                rect->y1;

            compositor_region_locked(
                0);
        }
    }

    if (direct_framebuffer) {
        __asm__ volatile (
            "sfence"
            :
            :
            : "memory");

        sched_preempt_enable();
    } else {
        compositor_present_reset_scanout_state();
        compositor_commit_snapshot();

        /*
         * Do not restore/reblend 24x24 cursor pixels after every unrelated
         * client update.  A coordinate change still presents immediately;
         * force only when this scene transaction actually overwrote the
         * currently visible cursor rectangle.
         */
        compositor_present_cursor_direct(
            compositor_present_scanout_flipped() ||
            compositor_snapshot_overwrites_presented_cursor());
    }
}

void compositor_snapshot_finish(void) {
    /*
     * This snapshot has finished scene rendering and publication.
     *
     * Record the exact geometry that is now visually committed before
     * allowing a future HID drag to consider retained reuse.  Logical window
     * state may already have advanced on another CPU; only these snapshot
     * coordinates are safe as a memmove source.
     */
    window_lock();

    for (uint32_t index = 0U;
         index < g_compositor_snapshot.window_count;
         ++index) {

        compositor_window_view_t *view =
            &g_compositor_snapshot.windows[index];

        window_server_window_t *reference =
            view->reference;

        if (reference == 0) {
            continue;
        }

        reference->compositor_presented_x =
            view->x;

        reference->compositor_presented_y =
            view->y;

        reference->compositor_presented_width =
            window_outer_width(
                view->width,
                view->flags);

        reference->compositor_presented_height =
            window_outer_height(
                view->height,
                view->flags);

        reference->compositor_presented_valid =
            true;
    }

    /*
     * drag_blit_valid describes only the pending snapshot transaction.  Clear
     * it after commit; the next motion independently derives reuse eligibility
     * from compositor_presented_* plus dirty/composing state.
     */
    window_input_set_drag_blit_valid_locked(false);

    g_window_server.composing = false;

    window_unlock();

    /*
     * Release the unlocked compositor's lifetime references only after the
     * committed-geometry bookkeeping above.  object_put() remains outside
     * window_lock so destruction cannot lengthen the scene critical section.
     */
    for (uint32_t index = 0U;
         index < g_compositor_snapshot.window_count;
         ++index) {

        compositor_window_view_t *view =
            &g_compositor_snapshot.windows[index];

        if (view->reference != 0) {
            object_put(view->reference);
            view->reference = 0;
        }

        view->section = 0;
    }

    g_compositor_snapshot.window_count = 0U;
    g_compositor_snapshot.damage_count = 0U;

    compositor_render_plan_reset();

    /*
     * Do not clear producer dirty/damage here.
     *
     * Input, resize, WINDOW_UPDATE or process exit may have generated another
     * frame while this snapshot was rendering.
     */
}

void compositor_region_locked(
    const compositor_render_span_t *planned_span) {
    uint32_t damage_left =
        g_compositor_snapshot.damage_bounds.x0;

    uint32_t damage_top =
        g_compositor_snapshot.damage_bounds.y0;

    uint32_t damage_right =
        g_compositor_snapshot.damage_bounds.x1;

    uint32_t damage_bottom =
        g_compositor_snapshot.damage_bounds.y1;

    if (damage_left >= damage_right ||
        damage_top >= damage_bottom) {
        return;
    }

    /*
     * Desktop rendering is now also based on snapshot damage, pointer and
     * hover state.  It does not consume mutable scene state.
     */
    uint32_t first_window = 0U;

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


    if (planned_span != 0) {
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

    /*
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
     *
     * All dirty regions are first completed in composite_framebuffer.
     * compositor_commit_snapshot() publishes the entire frame transaction
     * after every region is ready.
    */
}
