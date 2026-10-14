#include <arch/x86_64/cpu.h>
#include <kernel/mm.h>
#include <kernel/sched.h>
#include <kernel/timer.h>
#include <kernel/bitmap.h>

static spinlock_t g_timer_lock;
static atomic_uint g_timer_init_state;
static timer_object_t *g_timers[TIMER_OBJECT_LIMIT];
/* Exclusive upper occupied slot, protected/published with g_timer_lock. */
static uint32_t g_timer_scan_limit;
#define TIMER_SLOT_WORDS ((TIMER_OBJECT_LIMIT + 63U) / 64U)
static uint64_t g_timer_occupied[TIMER_SLOT_WORDS]; /* protected by g_timer_lock */

static uint32_t timer_find_free_slot_locked(void) {
    size_t slot = bitmap_find_first_zero(g_timer_occupied, TIMER_OBJECT_LIMIT);
    return slot < TIMER_OBJECT_LIMIT ? (uint32_t)slot : TIMER_OBJECT_LIMIT;
}

static uint32_t timer_registered_limit_locked(void) {
    for (uint32_t word = TIMER_SLOT_WORDS; word != 0U;) {
        --word;
        uint64_t occupied = g_timer_occupied[word];
        if (occupied != 0U)
            return word * 64U + bit_scan_reverse_u64(occupied) + 1U;
    }
    return 0U;
}

static void timer_global_lock(void) {
    /*
     * timer_poll() runs as deferred kernel work and may hold this lock while
     * notifying waiters.  Keep its owner on the CPU until the table walk and
     * wakeups are published; otherwise a timer/object cleanup on that same
     * CPU can preempt it and spin forever on this global lock.
     */
    sched_preempt_disable();
    spinlock_lock(&g_timer_lock);
}

static void timer_global_unlock(void) {
    spinlock_unlock(&g_timer_lock);
    sched_preempt_enable();
}

static void timer_global_initialize(void) {
    if (atomic_load_explicit(&g_timer_init_state, memory_order_acquire) == 2U) return;
    unsigned expected = 0U;
    if (atomic_compare_exchange_strong_explicit(&g_timer_init_state, &expected, 1U,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        spinlock_init(&g_timer_lock);
        for (uint32_t i = 0; i < TIMER_OBJECT_LIMIT; ++i) g_timers[i] = 0;
        g_timer_scan_limit = 0U;
        bitmap_zero(g_timer_occupied, TIMER_OBJECT_LIMIT);
        atomic_store_explicit(&g_timer_init_state, 2U, memory_order_release);
        return;
    }
    while (atomic_load_explicit(&g_timer_init_state, memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
}

static bool timer_is_signaled(const void *object) {
    const timer_object_t *timer = (const timer_object_t *)object;
    return atomic_load_explicit(&timer->fired, memory_order_acquire) ||
           atomic_load_explicit(&timer->canceled, memory_order_acquire);
}

static int64_t timer_wait_value(const void *object) {
    const timer_object_t *timer = (const timer_object_t *)object;
    if (atomic_load_explicit(&timer->canceled, memory_order_acquire)) return K_ECANCELED;
    return atomic_load_explicit(&timer->fired, memory_order_acquire) ? 1 : 0;
}

static void timer_destroy(void *object) {
    timer_object_t *timer = (timer_object_t *)object;
    (void)timer_cancel(timer);
    timer_global_initialize();
    timer_global_lock();
    uint32_t slot = timer->registry_slot;
    if (slot < g_timer_scan_limit && g_timers[slot] == timer) {
        g_timers[slot] = 0;
        bitmap_clear_bit(g_timer_occupied, slot);
    } else {
        /* Recover stale/unregistered slot metadata without leaving a
         * dangling registry pointer when the object is freed below. */
        for (uint32_t i = 0; i < g_timer_scan_limit; ++i) {
            if (g_timers[i] == timer) {
                g_timers[i] = 0;
                bitmap_clear_bit(g_timer_occupied, i);
            }
        }
    }
    timer->registry_slot = TIMER_OBJECT_LIMIT;
    g_timer_scan_limit = timer_registered_limit_locked();
    timer_global_unlock();
    kfree(timer);
}

static void timer_handle_close(void *object) {
    (void)timer_cancel((timer_object_t *)object);
}

static const object_ops_t g_timer_ops = {
    .destroy = timer_destroy,
    .handle_close = timer_handle_close,
    .type_name = "Timer",
    .is_signaled = timer_is_signaled,
    .wait_value = timer_wait_value,
};

static bool timer_deadline_reached(uint64_t now, uint64_t deadline) {
    return (int64_t)(now - deadline) >= 0;
}

kstatus_t timer_create(uint64_t delay_ns, uint64_t period_ns,
                       timer_object_t **out) {
    if (out == 0 || (period_ns != 0 && period_ns < 1000ULL)) return K_EINVAL;
    timer_global_initialize();
    timer_object_t *timer = (timer_object_t *)kzalloc(sizeof(*timer), 0);
    if (timer == 0) return K_ENOMEM;
    object_header_init(&timer->object, KOBJECT_TYPE_TIMER, &g_timer_ops);
    atomic_init(&timer->canceled, false);
    atomic_init(&timer->fired, false);
    timer->registry_slot = TIMER_OBJECT_LIMIT;
    uint64_t now = x86_read_tsc();
    uint64_t delay_tsc = x86_timeout_ns_to_tsc(delay_ns);
    timer->deadline_tsc = delay_tsc > UINT64_MAX - now ? UINT64_MAX : now + delay_tsc;
    timer->period_tsc = x86_timeout_ns_to_tsc(period_ns);

    timer_global_lock();
    uint32_t slot = timer_find_free_slot_locked();
    if (slot != TIMER_OBJECT_LIMIT) {
        timer->registry_slot = slot;
        g_timers[slot] = timer;
        bitmap_set_bit(g_timer_occupied, slot);
        if (slot == g_timer_scan_limit) ++g_timer_scan_limit;
    }
    timer_global_unlock();
    if (slot == TIMER_OBJECT_LIMIT) {
        kfree(timer);
        return K_ENOMEM;
    }
    *out = timer;
    return K_OK;
}

kstatus_t timer_cancel(timer_object_t *timer) {
    if (timer == 0 || timer->object.type != KOBJECT_TYPE_TIMER) return K_EINVAL;
    /* Cancellation is persistent readiness. Publish/notify only on the
     * transition: repeated callers and final destruction need no new wake. */
    if (!atomic_exchange_explicit(&timer->canceled, true, memory_order_acq_rel))
        object_notify_signaled(timer);
    return K_OK;
}

void timer_poll(uint64_t now_tsc) {
    timer_global_initialize();
    timer_global_lock();
    /* Registration and retirement hold this same lock. Walk occupied bits
     * in slot order, preserving notification order without reading holes. */
    uint32_t words = (g_timer_scan_limit + 63U) / 64U;
    for (uint32_t word = 0; word < words; ++word) {
        uint64_t occupied = g_timer_occupied[word];
        while (occupied != 0U) {
            uint32_t i = word * 64U + bit_scan_forward_u64(occupied);
            occupied &= occupied - 1U;
            timer_object_t *timer = g_timers[i];
            if (timer == 0 || atomic_load_explicit(&timer->canceled, memory_order_acquire)) {
                continue;
            }
            /* fired is persistent readiness, not a consumable token. All pollers
             * serialize on the global lock, and only polling publishes fired;
             * completed one-shot timers need no further locks/notifications. */
            if (timer->period_tsc == 0U &&
                atomic_load_explicit(&timer->fired, memory_order_acquire)) continue;
            /* Creation and deadline updates also serialize on the global lock.
             * Future deadlines need no per-object lock on every timer tick. */
            if (!timer_deadline_reached(now_tsc, timer->deadline_tsc)) continue;
            /* The global lock protects lifetime and deadline updates. fired and
             * canceled remain atomic for waiters/cancelers outside this lock. */
            atomic_store_explicit(&timer->fired, true, memory_order_release);
            if (timer->period_tsc != 0) {
                uint64_t next = timer->deadline_tsc + timer->period_tsc;
                timer->deadline_tsc = next < timer->deadline_tsc ? UINT64_MAX : next;
            }
            object_notify_signaled(timer);
        }
    }
    timer_global_unlock();
}

bool timer_self_test(void) {
    timer_object_t *timer = 0;
    if (timer_create(0, 0, &timer) != K_OK || timer == 0) return false;
    timer_poll(x86_read_tsc());
    unsigned notifications = atomic_load_explicit(
        &timer->object.signal_waiters.sequence, memory_order_acquire);
    for (unsigned i = 0U; i < 64U; ++i) timer_poll(x86_read_tsc());
    bool success = object_is_signaled(timer) &&
                   notifications == 1U &&
                   atomic_load_explicit(&timer->object.signal_waiters.sequence,
                                        memory_order_acquire) == notifications &&
                   object_wait_value(timer) == 1 &&
                   timer_cancel(timer) == K_OK &&
                   object_wait_value(timer) == K_ECANCELED;
    unsigned canceled_notifications = atomic_load_explicit(
        &timer->object.signal_waiters.sequence, memory_order_acquire);
    for (unsigned i = 0U; i < 64U; ++i)
        success = timer_cancel(timer) == K_OK && success;
    success = success && canceled_notifications == notifications + 1U &&
        atomic_load_explicit(&timer->object.signal_waiters.sequence,
                             memory_order_acquire) == canceled_notifications;
    object_put(timer);
    return success;
}
