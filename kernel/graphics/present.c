#include <arch/x86_64/context.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/smp.h>
#include <kernel/display.h>
#include <kernel/sched.h>
#include <kernel/qemu_stdvga.h>
#include "internal.h"

#ifndef WINDOW_COMPOSITOR_PARALLEL_PIXELS
#define WINDOW_COMPOSITOR_PARALLEL_PIXELS (512U * 1024U)
#endif

#ifndef WINDOW_COMPOSITOR_COPY_COMPLETION_SPINS
#define WINDOW_COMPOSITOR_COPY_COMPLETION_SPINS 1024U
#endif

#define WINDOW_COMPOSITOR_COPY_MAX_WORKERS 2U
#define WINDOW_COMPOSITOR_COPY_STACK_SIZE (32U * 1024U)

/* True only for the scene frame switched through hidden StdVGA VRAM. */
bool g_compositor_scanout_flipped;

void compositor_present_reset_scanout_state(void) {
    g_compositor_scanout_flipped = false;
}

void compositor_present_mark_scanout_flipped(void) {
    g_compositor_scanout_flipped = true;
}

bool compositor_present_scanout_flipped(void) {
    return g_compositor_scanout_flipped;
}

/*
 * The scene is still composed by one owner, but the expensive final copy from
 * the retained WB buffer to the WC GOP mapping is safely parallelizable by
 * scanline.  Workers never share a row, and each worker executes an SFENCE
 * before publishing completion.  This keeps the framebuffer transaction
 * coherent while allowing large drag rectangles to use the other CPUs.
 */
typedef struct compositor_copy_job {
    volatile uint32_t *destination;
    const uint32_t *source;
    uint32_t destination_stride;
    uint32_t source_stride;
    Rect rect;
} compositor_copy_job_t;

static compositor_copy_job_t g_compositor_copy_job;
static wait_queue_t g_compositor_copy_waitq;
static atomic_uint_fast64_t g_compositor_copy_generation;
static atomic_uint g_compositor_copy_completed;
static atomic_uint g_compositor_copy_worker_count;
static atomic_bool g_compositor_copy_workers_started;
static thread_t g_compositor_copy_workers[WINDOW_COMPOSITOR_COPY_MAX_WORKERS];
static uint8_t g_compositor_copy_worker_stacks
    [WINDOW_COMPOSITOR_COPY_MAX_WORKERS]
    [WINDOW_COMPOSITOR_COPY_STACK_SIZE]
    __attribute__((aligned(16)));

void compositor_present_init(void) {
    wait_queue_init(&g_compositor_copy_waitq);
    atomic_init(&g_compositor_copy_generation, 0U);
    atomic_init(&g_compositor_copy_completed, 0U);
    atomic_init(&g_compositor_copy_worker_count, 0U);
    atomic_init(&g_compositor_copy_workers_started, false);
}

void compositor_copy_wc_scanline(
    volatile uint32_t *__restrict destination,
    const uint32_t *__restrict source,
    uint32_t pixels) {

    display_core_publish_xrgb8888_span(
        destination,
        source,
        pixels);
}

typedef struct compositor_copy_wait_context {
    uint64_t generation;
} compositor_copy_wait_context_t;

static bool compositor_copy_worker_ready(void *context) {
    compositor_copy_wait_context_t *wait =
        (compositor_copy_wait_context_t *)context;
    return wait != 0 &&
           atomic_load_explicit(&g_compositor_copy_generation,
                                memory_order_acquire) != wait->generation;
}

static void compositor_copy_rows(const compositor_copy_job_t *job,
                                 uint32_t participant,
                                 uint32_t participants) {
    uint64_t height;
    uint64_t rows_per_participant;
    uint64_t first_row;
    uint64_t last_row;

    if (job == 0 || participants == 0U ||
        job->destination == 0 || job->source == 0 ||
        job->rect.x0 >= job->rect.x1 ||
        job->rect.y0 >= job->rect.y1 ||
        participant >= participants) {
        return;
    }

    /*
     * Give each CPU one contiguous scanline range.
     *
     * The old participant, participant+N, ... interleave makes every CPU jump
     * through the source and WC destination with an N-row stride.  Equal
     * contiguous slices preserve sequential WB reads and WC write combining.
     */
    height =
        (uint64_t)job->rect.y1 -
        job->rect.y0;

    rows_per_participant =
        (height + participants - 1U) /
        participants;

    first_row =
        (uint64_t)job->rect.y0 +
        rows_per_participant * participant;

    if (first_row >= (uint64_t)job->rect.y1) {
        return;
    }

    last_row =
        first_row +
        rows_per_participant;

    if (last_row > (uint64_t)job->rect.y1) {
        last_row = (uint64_t)job->rect.y1;
    }

    for (uint64_t row = first_row;
         row < last_row;
         ++row) {
        volatile uint32_t *destination =
            job->destination + row * job->destination_stride +
            job->rect.x0;
        const uint32_t *source =
            job->source + row * job->source_stride + job->rect.x0;
        compositor_copy_wc_scanline(destination, source,
                                     job->rect.x1 - job->rect.x0);
    }
}

static void __attribute__((noreturn)) compositor_copy_worker_main(void *argument) {
    uint32_t worker_index = (uint32_t)(uintptr_t)argument;
    uint64_t generation = atomic_load_explicit(
        &g_compositor_copy_generation, memory_order_acquire);
    uint32_t worker_count = atomic_load_explicit(
        &g_compositor_copy_worker_count, memory_order_acquire);

    for (;;) {
        compositor_copy_wait_context_t wait_context = {
            .generation = generation,
        };

        if (wait_on_queue(&g_compositor_copy_waitq,
                          compositor_copy_worker_ready,
                          &wait_context, UINT64_MAX) != K_OK) {
            continue;
        }

        generation = atomic_load_explicit(
            &g_compositor_copy_generation, memory_order_acquire);
        worker_count = atomic_load_explicit(
            &g_compositor_copy_worker_count, memory_order_acquire);
        if (worker_count == 0U || worker_index >= worker_count) continue;

        /* Participant zero is the compositor itself; every participant owns
         * one disjoint contiguous row slice. */
        compositor_copy_rows(&g_compositor_copy_job,
                             worker_index + 1U,
                             worker_count + 1U);
        __asm__ volatile ("sfence" : : : "memory");
        atomic_fetch_add_explicit(&g_compositor_copy_completed, 1U,
                                  memory_order_release);
    }
}

bool compositor_present_start_copy_workers(uint32_t compositor_cpu) {
    uint32_t cpu_ids[WINDOW_COMPOSITOR_COPY_MAX_WORKERS];
    uint32_t count = 0U;
    uint32_t current_cpu = x86_current_cpu_index();

    if (atomic_load_explicit(&g_compositor_copy_workers_started,
                             memory_order_acquire)) {
        return true;
    }

    for (uint32_t candidate = 0U;
         candidate < x86_smp_discovered_count() && candidate < MAX_CPUS;
         ++candidate) {
        if (candidate == compositor_cpu ||
            candidate == current_cpu ||
            !x86_smp_cpu_online(candidate)) {
            continue;
        }
        cpu_ids[count++] = candidate;
        if (count == WINDOW_COMPOSITOR_COPY_MAX_WORKERS) break;
    }

    if (count == 0U) return false;

    atomic_store_explicit(&g_compositor_copy_worker_count, count,
                          memory_order_release);
    atomic_store_explicit(&g_compositor_copy_workers_started, true,
                          memory_order_release);

    for (uint32_t index = 0U; index < count; ++index) {
        thread_t *worker = &g_compositor_copy_workers[index];
        uint8_t *worker_bytes = (uint8_t *)worker;
        for (size_t byte = 0U; byte < sizeof(*worker); ++byte) {
            worker_bytes[byte] = 0U;
        }

        refcount_init(&worker->object.refs, 1U);
        worker->object.type = KOBJECT_TYPE_THREAD;
        worker->object.flags = 0U;
        worker->object.ops = 0;
        worker->tid = UINT64_MAX - 2ULL - index;
        worker->process = 0;
        atomic_init(&worker->state, THREAD_READY);
        atomic_init(&worker->block_epoch, 0U);
        atomic_init(&worker->command_ack, 0U);
        worker->kernel_stack_base = g_compositor_copy_worker_stacks[index];
        worker->kernel_stack_size = WINDOW_COMPOSITOR_COPY_STACK_SIZE;
        worker->kernel_stack_top =
            ((vaddr_t)(uintptr_t)worker->kernel_stack_base +
             worker->kernel_stack_size) & ~(vaddr_t)0x0FULL;
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
        worker->affinity.bits[cpu_ids[index] >> 6] =
            1ULL << (cpu_ids[index] & 63U);
        worker->owner_cpu = (uint16_t)cpu_ids[index];
        worker->current_cpu = (uint16_t)cpu_ids[index];

        uintptr_t stack_top = (uintptr_t)worker->kernel_stack_top;
        uintptr_t switch_stack = stack_top - sizeof(uint64_t);
        *(uint64_t *)switch_stack =
            (uint64_t)(uintptr_t)&x86_kernel_thread_start;
        worker->arch.switch_ctx.rsp = switch_stack;
        worker->arch.switch_ctx.r12 =
            (uint64_t)(uintptr_t)&compositor_copy_worker_main;
        worker->arch.switch_ctx.r13 = (uint64_t)(uintptr_t)index;
        worker->arch.switch_ctx.r14 = stack_top;
        worker->arch.fs_base = 0U;

        sched_enqueue_bootstrap(worker);
    }

    /* If a helper landed on this CPU, let it reach its wait state before the
     * first frame.  Remote helpers are started by their reschedule IPIs. */
    for (uint32_t index = 0U; index < count; ++index) {
        if (cpu_ids[index] == current_cpu) {
            (void)sched_try_run_ready();
            break;
        }
    }
    return true;
}

bool compositor_copy_rect_parallel_buffers(
    volatile uint32_t *destination,
    const uint32_t *source,
    uint32_t destination_stride,
    uint32_t source_stride,
    const Rect *rect) {
    uint32_t worker_count = atomic_load_explicit(
        &g_compositor_copy_worker_count, memory_order_acquire);
    uint32_t participants;
    uint32_t completion_spins = 0U;
    uint64_t generation;
    uint64_t pixels;

    if (!atomic_load_explicit(&g_compositor_copy_workers_started,
                              memory_order_acquire) ||
        sched_preempt_disabled() ||
        worker_count == 0U || rect == 0 ||
        rect->x0 >= rect->x1 || rect->y0 >= rect->y1) {
        return false;
    }

    pixels = (uint64_t)(rect->x1 - rect->x0) *
             (uint64_t)(rect->y1 - rect->y0);
    if (pixels <= WINDOW_COMPOSITOR_PARALLEL_PIXELS) return false;

    participants = worker_count + 1U;
    g_compositor_copy_job.destination = destination;
    g_compositor_copy_job.source = source;
    g_compositor_copy_job.destination_stride = destination_stride;
    g_compositor_copy_job.source_stride = source_stride;
    g_compositor_copy_job.rect = *rect;
    atomic_store_explicit(&g_compositor_copy_completed, 0U,
                          memory_order_relaxed);
    generation = atomic_load_explicit(&g_compositor_copy_generation,
                                      memory_order_relaxed) + 1U;
    atomic_store_explicit(&g_compositor_copy_generation, generation,
                          memory_order_release);
    (void)wake_all(&g_compositor_copy_waitq);

    /*
     * Participant zero owns the first contiguous row slice; helper CPUs own
     * the following disjoint slices.
     */
    compositor_copy_rows(&g_compositor_copy_job, 0U, participants);
    __asm__ volatile ("sfence" : : : "memory");

    while (atomic_load_explicit(&g_compositor_copy_completed,
                                memory_order_acquire) < worker_count) {
        /*
         * In the normal case remote helpers started at the same generation and
         * are only slightly behind participant zero.  A short PAUSE phase is
         * cheaper than immediately descheduling the compositor.
         *
         * If a helper really is delayed, periodically yield so this never
         * becomes an unbounded busy wait.
         */
        if (completion_spins <
            WINDOW_COMPOSITOR_COPY_COMPLETION_SPINS) {
            ++completion_spins;
            __asm__ volatile ("pause");
            continue;
        }

        completion_spins = 0U;
        schedule();
    }

    /*
     * Participant zero fenced before waiting.  Every helper fences before its
     * release-completion increment, so the acquire above already observes a
     * completed WC transaction; another local SFENCE cannot order remote
     * stores any further.
     */
    return true;
}

bool compositor_copy_rect_parallel(const Rect *rect) {
    return compositor_copy_rect_parallel_buffers(
        g_window_server.framebuffer,
        (const uint32_t *)(uintptr_t)
            g_window_server.composite_framebuffer,
        g_window_server.display_stride,
        g_window_server.display_stride,
        rect);
}


/*
 * LITEOS_QEMU_INCREMENTAL_REPAIR_V1
 *
 * Each QEMU hidden-VRAM page is a retained scanout cache.
 *
 * A page can be one frame behind while it is hidden, so damage produced by a
 * completed scene transaction is accumulated for BOTH pages.  Only the page
 * selected as the current back page is repaired before the Y_OFFSET flip.
 *
 * The software cursor is not part of composite_framebuffer.  It is drawn
 * directly into the visible WC page, therefore the current front page also
 * records the cursor rectangle as damage which must be removed the next time
 * that physical page becomes the back page.
 *
 * Keep a small rectangle set instead of one global bounding rectangle.  This
 * prevents a tiny cursor update in one corner plus unrelated client damage in
 * another corner from degenerating into a near-full-screen copy.
 */
#define WINDOW_QEMU_REPAIR_MAX_RECTS 32U

typedef struct compositor_qemu_repair_set {
    Rect rects[WINDOW_QEMU_REPAIR_MAX_RECTS];
    uint32_t count;
} compositor_qemu_repair_set_t;

typedef struct compositor_qemu_repair_state {
    compositor_qemu_repair_set_t sets[2];

    /*
     * These are logical front/back ownership slots.  They are swapped only
     * after qemu_stdvga_flip() succeeds, matching the driver's front_page^1
     * transition without requiring the page index to escape the driver.
     */
    uint32_t front_slot;
    uint32_t back_slot;

    uint32_t width;
    uint32_t height;
    uint32_t stride;

    bool initialized;
} compositor_qemu_repair_state_t;

static compositor_qemu_repair_state_t
    g_compositor_qemu_repair;

static inline bool compositor_qemu_repair_mergeable(
    const Rect *a,
    const Rect *b) {

    /*
     * Half-open rectangles.  Treat touching edges as mergeable so sequential
     * drag strips collapse into one transaction instead of exhausting the
     * small repair list.
     */
    return
        a->x0 <= b->x1 &&
        b->x0 <= a->x1 &&
        a->y0 <= b->y1 &&
        b->y0 <= a->y1;
}

static inline void compositor_qemu_repair_union(
    Rect *destination,
    const Rect *source) {

    if (source->x0 < destination->x0)
        destination->x0 = source->x0;

    if (source->y0 < destination->y0)
        destination->y0 = source->y0;

    if (source->x1 > destination->x1)
        destination->x1 = source->x1;

    if (source->y1 > destination->y1)
        destination->y1 = source->y1;
}

static void compositor_qemu_repair_add(
    compositor_qemu_repair_set_t *set,
    const Rect *input) {

    Rect merged;

    if (set == 0 || input == 0)
        return;

    merged = *input;

    if (merged.x1 > (int32_t)g_window_server.display_width)
        merged.x1 = g_window_server.display_width;

    if (merged.y1 > (int32_t)g_window_server.display_height)
        merged.y1 = g_window_server.display_height;

    if (merged.x0 >= merged.x1 ||
        merged.y0 >= merged.y1) {
        return;
    }

    /*
     * Remove every rectangle touching the new one, expanding merged as we go.
     * Restart after every merge because the enlarged rectangle may now touch
     * an earlier entry.
     */
    for (;;) {
        bool changed = false;

        for (uint32_t index = 0U;
             index < set->count;
             ++index) {

            if (!compositor_qemu_repair_mergeable(
                    &merged,
                    &set->rects[index])) {
                continue;
            }

            compositor_qemu_repair_union(
                &merged,
                &set->rects[index]);

            --set->count;

            if (index != set->count) {
                set->rects[index] =
                    set->rects[set->count];
            }

            changed = true;
            break;
        }

        if (!changed)
            break;
    }

    if (set->count < WINDOW_QEMU_REPAIR_MAX_RECTS) {
        set->rects[set->count++] = merged;
        return;
    }

    /*
     * Pathological fragmentation.  Collapse to one bounding rectangle rather
     * than fall back to the old unconditional full-screen publication.
     */
    for (uint32_t index = 0U;
         index < set->count;
         ++index) {

        compositor_qemu_repair_union(
            &merged,
            &set->rects[index]);
    }

    set->rects[0] = merged;
    set->count = 1U;
}

static void compositor_qemu_repair_add_i64(
    compositor_qemu_repair_set_t *set,
    int64_t left,
    int64_t top,
    int64_t right,
    int64_t bottom) {

    Rect rect;

    if (left < 0)
        left = 0;

    if (top < 0)
        top = 0;

    if (right >
        (int64_t)g_window_server.display_width) {
        right = g_window_server.display_width;
    }

    if (bottom >
        (int64_t)g_window_server.display_height) {
        bottom = g_window_server.display_height;
    }

    if (left >= right ||
        top >= bottom) {
        return;
    }

    rect.x0 = (uint32_t)left;
    rect.y0 = (uint32_t)top;
    rect.x1 = (uint32_t)right;
    rect.y1 = (uint32_t)bottom;

    compositor_qemu_repair_add(
        set,
        &rect);
}

static void compositor_qemu_repair_reset(void) {
    Rect full = {
        .x0 = 0U,
        .y0 = 0U,
        .x1 = g_window_server.display_width,
        .y1 = g_window_server.display_height,
    };

    for (size_t index = 0U;
         index < sizeof(g_compositor_qemu_repair); ++index) {
        ((uint8_t *)(void *)&g_compositor_qemu_repair)[index] = 0U;
    }

    g_compositor_qemu_repair.front_slot = 0U;
    g_compositor_qemu_repair.back_slot = 1U;

    g_compositor_qemu_repair.width =
        g_window_server.display_width;

    g_compositor_qemu_repair.height =
        g_window_server.display_height;

    g_compositor_qemu_repair.stride =
        g_window_server.display_stride;

    g_compositor_qemu_repair.initialized = true;

    /*
     * We do not know the historical contents of either VRAM page at the point
     * incremental tracking starts.  The first use of each physical page must
     * therefore establish one complete clean baseline.
     *
     * Result:
     *   first flip  = full repair
     *   second flip = full repair
     *   later flips = incremental only
     */
    compositor_qemu_repair_add(
        &g_compositor_qemu_repair.sets[0],
        &full);

    compositor_qemu_repair_add(
        &g_compositor_qemu_repair.sets[1],
        &full);
}

static void compositor_qemu_repair_ensure(void) {
    if (!g_compositor_qemu_repair.initialized ||
        g_compositor_qemu_repair.width !=
            g_window_server.display_width ||
        g_compositor_qemu_repair.height !=
            g_window_server.display_height ||
        g_compositor_qemu_repair.stride !=
            g_window_server.display_stride) {

        compositor_qemu_repair_reset();
    }
}

/*
 * Mark pixels dirtied by the direct software-cursor overlay.
 *
 * Only the current front page was modified.  When roles flip this repair set
 * follows that physical page into the back role and removes the stale cursor
 * before it can become visible again.
 */
void compositor_qemu_repair_mark_front_cursor(
    int64_t left,
    int64_t top,
    int64_t right,
    int64_t bottom) {

    if (!qemu_stdvga_flip_available())
        return;

    compositor_qemu_repair_ensure();

    compositor_qemu_repair_add_i64(
        &g_compositor_qemu_repair.sets[
            g_compositor_qemu_repair.front_slot],
        left,
        top,
        right,
        bottom);
}

/*
 * P21-MOVE QEMU StdVGA incremental hidden-frame publication.
 *
 * Old P14:
 *
 *     retained WB scene
 *            |
 *            +---- FULL SCREEN ----> hidden WC page
 *                                     |
 *                                    flip
 *
 * New path:
 *
 *     scene damage ----+--> pending physical page A
 *                      |
 *                      +--> pending physical page B
 *
 *     current back page:
 *         repair only its pending rectangles
 *         clear repaired state
 *         flip
 *
 * Each page therefore catches up directly from the authoritative retained
 * scene instead of being copied from the other VRAM page.
 */
static void compositor_qemu_repair_accumulate_scene(void) {
    compositor_qemu_repair_set_t *front;
    compositor_qemu_repair_set_t *back;

    compositor_qemu_repair_ensure();

    front =
        &g_compositor_qemu_repair.sets[
            g_compositor_qemu_repair.front_slot];

    back =
        &g_compositor_qemu_repair.sets[
            g_compositor_qemu_repair.back_slot];

    if (g_compositor_snapshot.dragging_identifier != 0U) {
        Rect transaction;
        bool valid = false;

        /*
         * Retained drag uses memmove for the large OLD/NEW overlap and then
         * rerenders only sparse exposed strips.  Those sparse rectangles alone
         * are NOT enough to synchronize a VRAM page which is one frame behind.
         *
         * Use the same complete move transaction that scanout publication
         * logically represents.
         */
        for (uint32_t index = 0U;
             index < g_compositor_snapshot.damage_count;
             ++index) {

            const Rect *rect =
                &g_compositor_snapshot.damage_rects[index];

            if (rect->x0 >= rect->x1 ||
                rect->y0 >= rect->y1) {
                continue;
            }

            if (!valid) {
                transaction = *rect;
                valid = true;
            } else {
                compositor_qemu_repair_union(
                    &transaction,
                    rect);
            }
        }

        if (valid) {
            compositor_qemu_repair_add(
                front,
                &transaction);

            compositor_qemu_repair_add(
                back,
                &transaction);
        }

        return;
    }

    /*
     * Ordinary updates preserve their sparse rectangles.  This is important
     * for cursor-sized/client-sized changes far away from each other.
     */
    for (uint32_t index = 0U;
         index < g_compositor_snapshot.damage_count;
         ++index) {

        const Rect *rect =
            &g_compositor_snapshot.damage_rects[index];

        compositor_qemu_repair_add(
            front,
            rect);

        compositor_qemu_repair_add(
            back,
            rect);
    }
}

static void compositor_qemu_copy_repair_set(
    volatile uint32_t *destination,
    const uint32_t *source,
    const compositor_qemu_repair_set_t *repair) {

    if (destination == 0 ||
        source == 0 ||
        repair == 0) {
        return;
    }

    for (uint32_t index = 0U;
         index < repair->count;
         ++index) {

        const Rect *rect =
            &repair->rects[index];

        if (rect->x0 >= rect->x1 ||
            rect->y0 >= rect->y1) {
            continue;
        }

        /*
         * Large rectangles can still use the persistent helper CPUs.  Small
         * repairs deliberately stay on one CPU; waking remote workers for a
         * 64x64/24x24 rectangle costs more than the copy itself.
         */
        if (compositor_copy_rect_parallel_buffers(
                destination,
                source,
                g_window_server.display_stride,
                g_window_server.display_stride,
                rect)) {
            continue;
        }

        const uint32_t width =
            rect->x1 - rect->x0;

        for (uint32_t row = (uint32_t)rect->y0;
             row < (uint32_t)rect->y1;
             ++row) {

            compositor_copy_wc_scanline(
                destination +
                    (uint64_t)row *
                        g_window_server.display_stride +
                    rect->x0,
                source +
                    (uint64_t)row *
                        g_window_server.display_stride +
                    rect->x0,
                width);
        }
    }
}

bool compositor_commit_qemu_stdvga(void) {
    volatile uint32_t *back;
    volatile uint32_t *front;
    const uint32_t *scene;
    compositor_qemu_repair_set_t *repair;
    uint32_t repaired_slot;

    if (!qemu_stdvga_flip_available() ||
        g_window_server.composite_framebuffer == 0 ||
        g_window_server.composite_framebuffer ==
            g_window_server.framebuffer ||
        g_window_server.display_width == 0U ||
        g_window_server.display_height == 0U ||
        g_window_server.display_stride <
            g_window_server.display_width) {
        return false;
    }

    compositor_qemu_repair_accumulate_scene();

    repaired_slot =
        g_compositor_qemu_repair.back_slot;

    repair =
        &g_compositor_qemu_repair.sets[
            repaired_slot];

    back = qemu_stdvga_back_buffer();
    if (back == 0)
        return false;

    scene =
        (const uint32_t *)(uintptr_t)
            g_window_server.composite_framebuffer;

    /*
     * This is the entire expensive part now.  Once both VRAM pages have
     * received their initial full baseline, a drag copies only the accumulated
     * move transaction for this physical page.
     */
    compositor_qemu_copy_repair_set(
        back,
        scene,
        repair);

    /*
     * Local serial stores need this fence.  Parallel helpers already fence
     * their own WC stores before completion; the extra local fence is cheap
     * and also orders any serial rectangles following them.
     */
    __asm__ volatile ("sfence" : : : "memory");

    if (!qemu_stdvga_flip()) {
        /*
         * Do not discard repair state on failure.  The backend currently
         * disables itself after a verified flip failure, but retaining the
         * state also makes this correct if that policy changes later.
         */
        return false;
    }

    /*
     * The old hidden page is now front and exactly matches the clean retained
     * scene.  Its repair set becomes empty.  The old visible page becomes the
     * new back page and keeps all damage accumulated while it was visible.
     */
    g_compositor_qemu_repair.sets[
        repaired_slot].count = 0U;

    {
        uint32_t old_front =
            g_compositor_qemu_repair.front_slot;

        g_compositor_qemu_repair.front_slot =
            g_compositor_qemu_repair.back_slot;

        g_compositor_qemu_repair.back_slot =
            old_front;
    }

    front = qemu_stdvga_front_buffer();
    if (front == 0)
        return false;

    /*
     * Cursor-only HID updates write directly to this newly visible page.
     * compositor_present_cursor_direct(true) immediately follows this commit
     * and records that overlay into the new front page's repair set.
     */
    window_display_set_scanout_locked(front);

    return true;
}
