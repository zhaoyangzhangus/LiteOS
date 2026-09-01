#include <arch/x86_64/context.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/smp.h>
#include <kernel/console.h>
#include <kernel/perf.h>
#include <kernel/sched.h>
#include <kernel/telemetry.h>

#include <ascii_font.h>

#include "internal.h"

/* REFACTOR_P7A_SERVER_OWNER: state, lifecycle, worker, and readiness. */

#define WINDOW_SERVER_WORKER_STACK_SIZE (64U * 1024U)
atomic_bool g_window_dirty_hint __attribute__((aligned(64)));

atomic_uint_fast64_t g_window_dirty_generation
    __attribute__((aligned(64)));

atomic_uint_fast64_t g_window_dirty_notified_generation
    __attribute__((aligned(64)));

/* LiteOS desktop shell: wallpaper, icons and launcher. */
/*
 * Snapshot-local tile floor cache.
 *
 * The compositor is single-owner (g_window_server.composing), so this does not
 * need atomics.  Keep it outside compositor_snapshot_t so the already-large
 * immutable snapshot does not carry an extra 16 KiB through unrelated code.
 */
window_server_state_t g_window_server;

static uint8_t g_window_server_worker_stack[WINDOW_SERVER_WORKER_STACK_SIZE]
    __attribute__((aligned(16)));

static atomic_uint g_window_server_init_state;
static atomic_bool g_window_worker_running_reported;

void window_mark_dirty_locked(void);
void window_mark_rect_locked(int32_t x, int32_t y,
                             uint32_t width, uint32_t height);
void window_mark_window_locked(const window_server_window_t *window);
void window_mark_surface_locked(const window_server_window_t *window,
                                int32_t x, int32_t y,
                                uint32_t width, uint32_t height);
void window_mark_moved_rect_locked(int32_t old_x, int32_t old_y,
                                   int32_t new_x, int32_t new_y,
                                   uint32_t width, uint32_t height,
                                   bool render_new_position);
void window_mark_moved_cursor_locked(uint32_t old_x, uint32_t old_y,
                                     uint32_t new_x, uint32_t new_y);
void window_coalesce_damage_locked(void);
bool window_server_init(void) {
    unsigned expected = 0U;
    if (atomic_compare_exchange_strong_explicit(&g_window_server_init_state,
                                                &expected, 1U,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        atomic_init(&g_window_server.lock.state, 0U);
        wait_queue_init(&g_window_server.event_waitq);
        wait_queue_init(&g_window_server.worker_waitq);
        input_core_bind_wakeup(&g_window_server.worker_waitq);
        compositor_present_init();
        desktop_shell_init();
        atomic_init(&g_window_server.worker_generation, 0U);
        atomic_init(&g_window_dirty_hint, false);
        atomic_init(&g_window_dirty_generation, 0U);
        atomic_init(&g_window_dirty_notified_generation, 0U);
        window_event_reset_ready_locked();

        window_registry_reset_locked();
        window_scene_set_focus_identifier_locked(0U);
        g_window_server.manager = 0;
        window_display_reset_locked();
        window_buffer_reset_locked();
        window_input_set_pointer_locked(0U, 0U);
        window_present_cursor_reset_locked();
        desktop_set_hovered_app_locked(DESKTOP_APP_NONE);
        window_input_reset_desktop_state_locked();
        window_input_reset_drag_locked();
        window_clear_title_capture_locked();
        atomic_init(&g_window_server.worker_started, false);
        atomic_init(&g_window_server.desktop_asset_worker_started, false);
        atomic_init(&g_window_worker_running_reported, false);
        g_window_server.kernel_ready = false;
        compositor_reset_state_locked();
        window_damage_reset_locked();
        if (process_register_teardown_callback(window_server_close_process) !=
            K_OK) {
            atomic_store_explicit(&g_window_server_init_state, 0U,
                                  memory_order_release);
            return false;
        }
        atomic_store_explicit(&g_window_server_init_state, 2U,
                              memory_order_release);
        return true;
    }
    while (atomic_load_explicit(&g_window_server_init_state,
                                memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
    return true;
}

static bool window_server_worker_ready(void *context) {
    (void)context;

    /*
     * The worker is published before the first display mode is committed so
     * that later input can use the normal scheduler path.  Until the display
     * owner has finished window_server_kernel_ready(), however, dirty is only
     * bootstrap intent and compositor_frame_run() must not spin on it.
     */
    if (!g_window_server.kernel_ready) return false;

    if (input_core_pending() != 0U) return true;

    return atomic_load_explicit(&g_window_dirty_hint,
                                memory_order_acquire);
}

void window_server_notify_worker(void) {
    bool input_pending;

    if (!window_server_init() ||
        !atomic_load_explicit(
            &g_window_server.worker_started,
            memory_order_acquire)) {
        return;
    }

    input_pending =
        input_core_pending() != 0U;

    /*
     * Input is independent from scene dirty:
     * cursor-only HID motion must still wake immediately.
     */
    if (!input_pending &&
        atomic_load_explicit(
            &g_window_dirty_hint,
            memory_order_acquire)) {

        uint64_t generation =
            atomic_load_explicit(
                &g_window_dirty_generation,
                memory_order_acquire);

        uint64_t notified =
            atomic_load_explicit(
                &g_window_dirty_notified_generation,
                memory_order_relaxed);

        /*
         * Exactly one post-lock notifier wins for this scene generation.
         * Rendering is NOT delayed: the first clean->dirty producer still
         * wakes immediately.
         */
        for (;;) {
            if (notified == generation) {
                return;
            }

            if (atomic_compare_exchange_weak_explicit(
                    &g_window_dirty_notified_generation,
                    &notified,
                    generation,
                    memory_order_acq_rel,
                    memory_order_relaxed)) {
                break;
            }
        }
    }

    /*
     * Keep the existing counter as an actual-notification diagnostic.
     */
    atomic_fetch_add_explicit(
        &g_window_server.worker_generation,
        1U,
        memory_order_release);

    (void)wake_one(
        &g_window_server.worker_waitq);
}

/* The compositor sleeps indefinitely and is woken by input or scene damage.
 * There is no frame timer or idle scan; every pass has a producer. */
static void __attribute__((noreturn)) window_server_worker_main(void *argument) {
    (void)argument;
    if (!atomic_exchange_explicit(&g_window_worker_running_reported, true,
                                  memory_order_acq_rel)) {
        liteos_serial_printf_serial_only(
            "LITEOS_DIAG_WINDOW_WORKER_RUNNING CPU=%u\r\n",
            x86_current_cpu_index());
    }
    for (;;) {
        /* The worker is woken by both input reports and client
         * WINDOW_UPDATE submissions.  The predicate/blocked handshake in
         * wait_on_queue closes the producer-to-sleep lost-wakeup window. */
        if (wait_on_queue(&g_window_server.worker_waitq,
                          window_server_worker_ready, 0,
                          UINT64_MAX) != K_OK) continue;

        /*
         * Normal pointer-only motion keeps the direct cursor HID-rate path.
         *
         * Active drag uses ONE input pump with a larger budget. Keeping the
         * whole queued pure-motion run inside one window_motion_batch_t means
         * no intermediate dirty geometry is created before retained reuse is
         * decided.
         *
         * Compose the already-routed state directly. Calling another input
         * pump here would create a second geometry transition while dirty=1
         * and would invalidate the retained-scene proof.
         */
        for (;;) {
            bool defer_cursor_for_drag = false;

            if (input_core_pending() != 0U) {
                /*
                 * No FPS policy: consume currently queued input immediately.
                 * The existing motion batch coalesces only what is already
                 * pending; scene publication remains purely dirty-driven.
                 */
                window_server_pump_input_mode(false);

                window_lock();
                defer_cursor_for_drag =
                    g_window_server.dragging_identifier != 0U &&
                    g_window_server.dirty;
                window_unlock();

                if (!defer_cursor_for_drag) {
                    compositor_present_cursor_direct(false);
                }
            }

            if (!atomic_load_explicit(&g_window_dirty_hint,
                                      memory_order_acquire)) {
                break;
            }

            /*
             * Reports arriving during rendering stay queued for the next
             * frame, whose retained source will be this frame's committed
             * compositor_presented_* geometry.
             */
                    compositor_frame_run();
        }
    }
}

bool window_server_start_worker(void) {
    if (!window_server_init()) return false;
    if (atomic_load_explicit(&g_window_server.worker_started,
                             memory_order_acquire)) return true;

    thread_t *current = sched_current_thread();
    uint32_t cpu_id = x86_current_cpu_index();
    uint32_t worker_cpu = cpu_id;
    if (current == 0 || cpu_id >= MAX_CPUS) return false;

    /*
     * Keep the compositor away from the bootstrap CPU when an AP is online.
     * xHCI/deferred work and early user startup are concentrated on CPU0;
     * pinning the expensive compositor there makes pointer input wait behind
     * a full retained-buffer publication.  A single AP is sufficient because
     * window_lock still serializes scene mutation and composition.
     */
    for (uint32_t candidate = 0U;
         candidate < x86_smp_discovered_count() && candidate < MAX_CPUS;
         ++candidate) {
        if (candidate != cpu_id && x86_smp_cpu_online(candidate)) {
            worker_cpu = candidate;
            break;
        }
    }

    thread_t *worker = &g_window_server.worker;
    uint8_t *worker_bytes = (uint8_t *)worker;
    for (size_t i = 0U; i < sizeof(*worker); ++i) worker_bytes[i] = 0U;

    refcount_init(&worker->object.refs, 1U);
    worker->object.type = KOBJECT_TYPE_THREAD;
    worker->object.flags = 0U;
    worker->object.ops = 0;
    worker->tid = UINT64_MAX - 1ULL;
    worker->process = 0;
    atomic_init(&worker->state, THREAD_READY);
    worker->kernel_stack_base = g_window_server_worker_stack;
    worker->kernel_stack_size = sizeof(g_window_server_worker_stack);
    worker->kernel_stack_top =
        ((vaddr_t)(uintptr_t)g_window_server_worker_stack +
         sizeof(g_window_server_worker_stack)) & ~(vaddr_t)0x0FULL;
    /* Keep composition in the normal fair class.  A large retained-buffer
     * render must not starve the user applications that produce the next
     * frame; input wakeups are already prompt because the worker blocks when
     * idle and is directly notified by the input queue. */
    worker->sched_class = SCHED_CLASS_FAIR;
    worker->base_sched_class = SCHED_CLASS_FAIR;
    worker->rt_priority = 0U;
    worker->base_rt_priority = 0U;
    worker->sched.weight = 1024U;
    worker->sched.nice = 0;
    worker->sched.vruntime = 0U;
    list_init(&worker->sched.rt_node);
    list_init(&worker->process_node);
    list_init(&worker->owned_mutexes);
    for (uint32_t word = 0U; word < MAX_CPUS / 64U; ++word) {
        worker->affinity.bits[word] = 0U;
    }
    worker->affinity.bits[worker_cpu >> 6] = 1ULL << (worker_cpu & 63U);
    worker->current_cpu = (uint16_t)worker_cpu;

    uintptr_t stack_top = (uintptr_t)worker->kernel_stack_top;
    uintptr_t switch_stack = stack_top - sizeof(uint64_t);
    *(uint64_t *)switch_stack = (uint64_t)(uintptr_t)&x86_kernel_thread_start;
    worker->arch.switch_ctx.rsp = switch_stack;
    worker->arch.switch_ctx.r12 =
        (uint64_t)(uintptr_t)&window_server_worker_main;
    worker->arch.switch_ctx.r13 = 0U;
    worker->arch.switch_ctx.r14 = stack_top;
    worker->arch.fs_base = 0U;

    /*
     * The root assets are several megabytes each.  They are optional for the
     * first frame, so do not make service startup wait for three synchronous
     * VFS reads.  Publish a deterministic solid hold surface and let the
     * one-shot asset worker publish the real artwork asynchronously.
     */
    window_lock();
    window_mark_dirty_locked();
    window_unlock();

    atomic_store_explicit(&g_window_server.worker_started, true,
                          memory_order_release);
    if (!sched_enqueue_bootstrap(worker)) {
        atomic_store_explicit(&g_window_server.worker_started, false,
                              memory_order_release);
        return false;
    }
    /* Best effort: one compositor remains the scene owner; only the final
     * disjoint scanline publication is delegated to these helpers. */
    (void)compositor_present_start_copy_workers(worker_cpu);
    /* The next ordinary scheduling boundary starts a local compositor.
     * Never switch away from the bootstrap caller while publishing startup
     * state; on a single-CPU guest that would strand the remaining boot path. */
    return true;
}

bool window_server_start_asset_worker(void) {
    if (!window_server_init()) return false;
    if (desktop_shell_assets_available()) {
        return true;
    }
    if (!atomic_load_explicit(&g_window_server.worker_started,
                              memory_order_acquire)) {
        return false;
    }

    /*
     * The caller starts this only after /init-runtime has been loaded.  The
     * large artwork reads may still share the VFS with user services, but
     * they can no longer delay the boot-critical runtime hand-off.
     */
    return desktop_shell_start_asset_worker(
        g_window_server.worker.current_cpu);
}

bool window_server_kernel_ready(void) {
    uint64_t benchmark_start;

    if (!window_server_init()) return false;
    if (g_window_server.kernel_ready) return true;
    if (!ascii_font_load()) return false;
    if (!window_display_prepare()) return false;
    window_buffer_prepare();
    window_input_set_pointer_locked(
        g_window_server.display_width / 2U,
        g_window_server.display_height / 2U);
    window_lock();
    g_window_server.kernel_ready = true;
    window_mark_dirty_locked();
    window_unlock();

    /* Present the initial desktop immediately.  The compositor worker may
     * be placed on an AP and can legitimately remain asleep until its first
     * wake-up; boot must not require a keyboard or mouse report to show the
     * already-dirty desktop. */
    benchmark_start = telemetry_timestamp();
    window_server_pump_input_mode(true);
    /* The worker can already be blocked on its pre-display predicate. */
    window_server_notify_worker();
    kernel_perf_emit_scope("graphics.static_desktop_cpu", benchmark_start);
    return true;
}

/*
 * The server lock is the one synchronization boundary shared by window
 * mutation, input routing, damage publication, and compositor snapshots.
 * Keep the lock implementation independent from the transitional policy
 * implementation in compat/graphics/window_server.c.
 */
void window_lock(void) {
    sched_preempt_disable();

    for (;;) {
        if (atomic_exchange_explicit(&g_window_server.lock.state, 1U,
                                     memory_order_acquire) == 0U) {
            return;
        }

        /* Test-test-and-set keeps the cache line shared while contended. */
        while (atomic_load_explicit(&g_window_server.lock.state,
                                    memory_order_relaxed) != 0U) {
            __asm__ volatile ("pause");
        }
    }
}

void window_unlock(void) {
    atomic_store_explicit(&g_window_server.lock.state, 0U,
                          memory_order_release);
    sched_preempt_enable();
}
