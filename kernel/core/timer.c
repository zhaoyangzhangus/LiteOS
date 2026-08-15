#include <arch/x86_64/cpu.h>
#include <kernel/mm.h>
#include <kernel/timer.h>

static spinlock_t g_timer_lock;
static atomic_uint g_timer_init_state;
static timer_object_t *g_timers[TIMER_OBJECT_LIMIT];

static void timer_lock(timer_object_t *timer) {
    while (atomic_exchange_explicit(&timer->lock.state, 1U,
                                    memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
}

static void timer_unlock(timer_object_t *timer) {
    atomic_store_explicit(&timer->lock.state, 0U, memory_order_release);
}

static void timer_global_lock(void) {
    while (atomic_exchange_explicit(&g_timer_lock.state, 1U,
                                    memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
}

static void timer_global_unlock(void) {
    atomic_store_explicit(&g_timer_lock.state, 0U, memory_order_release);
}

static void timer_global_initialize(void) {
    unsigned expected = 0U;
    if (atomic_compare_exchange_strong_explicit(&g_timer_init_state, &expected, 1U,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        atomic_init(&g_timer_lock.state, 0U);
        for (uint32_t i = 0; i < TIMER_OBJECT_LIMIT; ++i) g_timers[i] = 0;
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
    for (uint32_t i = 0; i < TIMER_OBJECT_LIMIT; ++i) {
        if (g_timers[i] == timer) g_timers[i] = 0;
    }
    timer_global_unlock();
    kfree(timer);
}

static const object_ops_t g_timer_ops = {
    .destroy = timer_destroy,
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
    refcount_init(&timer->object.refs, 1U);
    timer->object.type = KOBJECT_TYPE_TIMER;
    timer->object.flags = 0;
    timer->object.ops = &g_timer_ops;
    timer->object.security = 0;
    atomic_init(&timer->lock.state, 0U);
    atomic_init(&timer->canceled, false);
    atomic_init(&timer->fired, false);
    uint64_t now = x86_read_tsc();
    uint64_t delay_tsc = x86_timeout_ns_to_tsc(delay_ns);
    timer->deadline_tsc = delay_tsc > UINT64_MAX - now ? UINT64_MAX : now + delay_tsc;
    timer->period_tsc = x86_timeout_ns_to_tsc(period_ns);

    timer_global_lock();
    uint32_t slot = TIMER_OBJECT_LIMIT;
    for (uint32_t i = 0; i < TIMER_OBJECT_LIMIT; ++i) {
        if (g_timers[i] == 0) {
            slot = i;
            break;
        }
    }
    if (slot != TIMER_OBJECT_LIMIT) g_timers[slot] = timer;
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
    atomic_store_explicit(&timer->canceled, true, memory_order_release);
    object_notify_signaled(timer);
    return K_OK;
}

void timer_poll(uint64_t now_tsc) {
    timer_global_initialize();
    timer_global_lock();
    for (uint32_t i = 0; i < TIMER_OBJECT_LIMIT; ++i) {
        timer_object_t *timer = g_timers[i];
        if (timer == 0 || atomic_load_explicit(&timer->canceled, memory_order_acquire)) {
            continue;
        }
        timer_lock(timer);
        if (timer_deadline_reached(now_tsc, timer->deadline_tsc)) {
            atomic_store_explicit(&timer->fired, true, memory_order_release);
            if (timer->period_tsc != 0) {
                uint64_t next = timer->deadline_tsc + timer->period_tsc;
                timer->deadline_tsc = next < timer->deadline_tsc ? UINT64_MAX : next;
            }
            object_notify_signaled(timer);
        }
        timer_unlock(timer);
    }
    timer_global_unlock();
}

bool timer_self_test(void) {
    timer_object_t *timer = 0;
    if (timer_create(0, 0, &timer) != K_OK || timer == 0) return false;
    timer_poll(x86_read_tsc());
    bool success = object_is_signaled(timer) &&
                   object_wait_value(timer) == 1 &&
                   timer_cancel(timer) == K_OK &&
                   object_wait_value(timer) == K_ECANCELED;
    object_put(timer);
    return success;
}
