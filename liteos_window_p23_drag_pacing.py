#!/usr/bin/env python3
from pathlib import Path
import shutil

SRC = Path("kernel/graphics/window_server.c")
s = SRC.read_text(encoding="utf-8")

SENTINEL = "LITEOS_COMPOSITOR_DRAG_FRAME_PACING_V1"

if SENTINEL in s:
    print("P23 drag frame pacing already installed")
    raise SystemExit(0)

backup = SRC.with_suffix(".c.before-p23-drag-frame-pacing")
if not backup.exists():
    shutil.copy2(SRC, backup)


def replace_once(text, old, new, name):
    if old not in text:
        raise SystemExit(f"ERROR: {name}: anchor not found")
    return text.replace(old, new, 1)


# ============================================================
# P23.1 - frame pacing policy
# ============================================================

s = replace_once(
    s,
    '''#ifndef WINDOW_COMPOSITOR_DRAG_INPUT_EVENTS
#define WINDOW_COMPOSITOR_DRAG_INPUT_EVENTS \\
    (WINDOW_EVENT_CAPACITY * 4U)
#endif

/* WB desktop-cache -> WB retained-scene copies must stay cached. */
''',
    '''#ifndef WINDOW_COMPOSITOR_DRAG_INPUT_EVENTS
#define WINDOW_COMPOSITOR_DRAG_INPUT_EVENTS \\
    (WINDOW_EVENT_CAPACITY * 4U)
#endif

/*
 * LITEOS_COMPOSITOR_DRAG_FRAME_PACING_V1
 *
 * The producer can consume a 1000 Hz mouse, but scanout should not publish a
 * complete software-drag layer 1000 times per second.
 *
 * 144 Hz is deliberately a compile-time policy until display_core exposes a
 * real mode refresh/vblank period. Set to 0 to disable pacing or override
 * from the build for 120/165/240 Hz displays.
 */
#ifndef WINDOW_COMPOSITOR_DRAG_MAX_FPS
#define WINDOW_COMPOSITOR_DRAG_MAX_FPS 144U
#endif

#if WINDOW_COMPOSITOR_DRAG_MAX_FPS != 0U
#define WINDOW_COMPOSITOR_DRAG_FRAME_NS \\
    (1000000000ULL / WINDOW_COMPOSITOR_DRAG_MAX_FPS)
#else
#define WINDOW_COMPOSITOR_DRAG_FRAME_NS 0ULL
#endif

/* WB desktop-cache -> WB retained-scene copies must stay cached. */
''',
    "P23 policy",
)


# ============================================================
# P23.2 - last successful drag publication timestamp
# ============================================================

s = replace_once(
    s,
    '''static atomic_bool g_window_dirty_hint __attribute__((aligned(64)));


/*
 * Flat modern Ring0 window controls.
''',
    '''static atomic_bool g_window_dirty_hint __attribute__((aligned(64)));

/*
 * Timestamp of the most recently completed drag/resize scene publication.
 *
 * Only the compositor normally writes this, but keep it atomic because the
 * initial/compatibility compose entry points are not required to execute on
 * the dedicated worker CPU.
 */
static atomic_uint_fast64_t g_window_drag_last_present_tsc
    __attribute__((aligned(64)));


/*
 * Flat modern Ring0 window controls.
''',
    "P23 timestamp state",
)


# ============================================================
# P23.3 - initialize pacing state
# ============================================================

s = replace_once(
    s,
    '''        atomic_init(&g_window_server.worker_generation, 0U);
        atomic_init(&g_window_dirty_hint, false);
        atomic_init(&g_compositor_copy_generation, 0U);
''',
    '''        atomic_init(&g_window_server.worker_generation, 0U);
        atomic_init(&g_window_dirty_hint, false);
        atomic_init(&g_window_drag_last_present_tsc, 0U);
        atomic_init(&g_compositor_copy_generation, 0U);
''',
    "P23 init",
)


# ============================================================
# P23.4 - blocking deadline helper
# ============================================================

worker_anchor = '''static void __attribute__((noreturn)) window_server_worker_main(void *argument) {
'''

if worker_anchor not in s:
    raise SystemExit("ERROR: worker main anchor not found")

helpers = r'''/*
 * A deliberately-false wait predicate.
 *
 * wait_on_queue() keeps one absolute deadline internally. Input/client wakes
 * during this pacing sleep therefore become harmless spurious wakeups: the
 * waiter rechecks false and blocks again until the ORIGINAL deadline.
 *
 * This is preferable to PAUSE/RDTSC spinning and does not consume a compositor
 * CPU while waiting for the next useful scanout time.
 */
static bool window_server_drag_frame_deadline_wait(void *context) {
    (void)context;
    return false;
}


/*
 * Return true only for a normal decoration move.
 *
 * Resize is intentionally excluded: resizing may require application-facing
 * size events and complete surface rerasterization, so it keeps the existing
 * low-latency immediate path.
 */
static bool window_server_move_drag_active(void) {
    bool active;

    window_lock();

    active =
        g_window_server.dragging_identifier != 0U &&
        g_window_server.resize_edges == 0U;

    window_unlock();

    return active;
}


/*
 * Block the compositor worker until the next useful drag publication slot.
 *
 * IMPORTANT ordering:
 *
 *     previous frame committed
 *             |
 *             v
 *     mouse wakes worker
 *             |
 *             v
 *     WAIT HERE
 *             |
 *        HID queue grows
 *             |
 *             v
 *     one window_server_pump_input_mode(false)
 *
 * We deliberately pace BEFORE consuming the next pure-motion run. Therefore
 * all reports accumulated inside one display interval still enter the existing
 * P13 window_motion_batch_t as ONE geometry transition. No intermediate
 * dirty=1 state is created and retained/P22 reuse remains valid.
 */
static void window_server_pace_move_drag(void) {
#if WINDOW_COMPOSITOR_DRAG_MAX_FPS != 0U
    uint64_t last;
    uint64_t interval;
    uint64_t deadline;
    uint64_t now;
    uint64_t remaining_ticks;
    uint64_t remaining_ns;

    /*
     * P22/P21 currently obtain their main bandwidth win only on the QEMU
     * hidden-page path. Do not impose an arbitrary 144 Hz policy on future
     * native GPU/vblank backends.
     */
    if (!qemu_stdvga_flip_available()) {
        return;
    }

    if (!window_server_move_drag_active()) {
        return;
    }

    last =
        atomic_load_explicit(
            &g_window_drag_last_present_tsc,
            memory_order_acquire);

    if (last == 0U) {
        return;
    }

    interval =
        x86_timeout_ns_to_tsc(
            WINDOW_COMPOSITOR_DRAG_FRAME_NS);

    if (interval == 0U) {
        return;
    }

    if (last > UINT64_MAX - interval) {
        return;
    }

    deadline =
        last + interval;

    now =
        x86_read_tsc();

    if ((int64_t)(now - deadline) >= 0) {
        return;
    }

    remaining_ticks =
        deadline - now;

    remaining_ns =
        x86_tsc_to_ns(
            remaining_ticks);

    /*
     * Integer conversion can round a tiny positive remainder to zero.
     */
    if (remaining_ns == 0U) {
        remaining_ns = 1U;
    }

    /*
     * K_ETIMEDOUT is the normal result. New HID reports may wake us early,
     * but the false predicate causes wait_on_queue() to re-block using its
     * original absolute deadline.
     */
    (void)wait_on_queue(
        &g_window_server.worker_waitq,
        window_server_drag_frame_deadline_wait,
        0,
        remaining_ns);
#else
    return;
#endif
}


'''

s = s.replace(
    worker_anchor,
    helpers + worker_anchor,
    1,
)


# ============================================================
# P23.5 - pace BEFORE the one P13 input pump
# ============================================================

s = replace_once(
    s,
    '''            if (input_core_pending() != 0U) {
                window_server_pump_input_mode(false);

                window_lock();
''',
    '''            if (input_core_pending() != 0U) {
                /*
                 * P23: leave reports in input_core until the next display
                 * deadline, then let the existing P13 pump merge all of them
                 * into one final X/Y transition.
                 */
                window_server_pace_move_drag();

                window_server_pump_input_mode(false);

                window_lock();
''',
    "P23 worker pacing call",
)


# ============================================================
# P23.6 - timestamp publication completion
# ============================================================

finish_anchor = '''static void compositor_snapshot_finish(void) {
    /*
     * This snapshot has finished scene rendering and publication.
'''

if finish_anchor not in s:
    raise SystemExit("ERROR: snapshot finish anchor not found")

s = s.replace(
    finish_anchor,
    '''static void compositor_snapshot_finish(void) {
    /*
     * P23 frame clock.
     *
     * Record after compositor_render_snapshot()/scanout publication returned,
     * not when input was consumed. The pacing interval is therefore measured
     * from actually completed visible work and cannot build an ever-growing
     * queue if one frame itself takes longer than the target period.
     */
    if (g_compositor_snapshot.dragging_identifier != 0U) {
        atomic_store_explicit(
            &g_window_drag_last_present_tsc,
            x86_read_tsc(),
            memory_order_release);
    } else {
        atomic_store_explicit(
            &g_window_drag_last_present_tsc,
            0U,
            memory_order_release);
    }

    /*
     * This snapshot has finished scene rendering and publication.
''',
    1,
)


SRC.write_text(s, encoding="utf-8")

print("OK: P23 drag frame pacing installed")
print(f"backup: {backup}")
print("default target: 144 Hz")
