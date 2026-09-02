#include "internal.h"

/*
 * LAPIC clock accounting and timeslice expiry live in this translation unit.
 * The timer only accounts execution and publishes a preemption request.  The
 * interrupt boundary decides whether to enter schedule().
 */
static uint64_t scheduler_fair_vruntime_delta(uint64_t runtime_delta,
                                              uint32_t weight) {
    if (runtime_delta == 0U) return 0U;
    if (weight == 0U) weight = 1U;

    /* Divide first so the common large-runtime case cannot overflow. */
    uint64_t quotient = runtime_delta / weight;
    uint64_t remainder = runtime_delta % weight;
    if (quotient > UINT64_MAX / SCHED_NICE_0_WEIGHT) return UINT64_MAX;
    uint64_t scaled = quotient * SCHED_NICE_0_WEIGHT;
    uint64_t fraction =
        remainder * SCHED_NICE_0_WEIGHT / weight;
    return scaled > UINT64_MAX - fraction ? UINT64_MAX : scaled + fraction;
}

static uint64_t scheduler_vruntime_add(uint64_t vruntime,
                                       uint64_t delta) {
    return vruntime > UINT64_MAX - delta ? UINT64_MAX : vruntime + delta;
}

void sched_tick(uint64_t now_ns) {
    uint32_t cpu_id;
    if (!scheduler_current_cpu(&cpu_id)) return;
    scheduler_cpu_t *cpu = &g_cpus[cpu_id];
    cpu->queue.clock_ns = now_ns;
    thread_t *current = cpu->queue.current;
    if (current == 0) return;
    if (current == cpu->queue.idle) {
        if (sched_runnable_count() != 0U) {
            atomic_store_explicit(&cpu->need_resched, true,
                                  memory_order_release);
        }
        return;
    }
    uint64_t delta = current->sched.exec_start_ns == 0 ?
                     0 : now_ns - current->sched.exec_start_ns;
    current->sched.runtime_ns = scheduler_vruntime_add(
        current->sched.runtime_ns, delta);
    current->sched.exec_start_ns = now_ns;
    if (current->sched_class == SCHED_CLASS_FAIR) {
        current->sched.vruntime = scheduler_vruntime_add(
            current->sched.vruntime,
            scheduler_fair_vruntime_delta(delta, current->sched.weight));
    } else if (current->sched_class == SCHED_CLASS_RT) {
        current->sched.slice_runtime_ns = scheduler_vruntime_add(
            current->sched.slice_runtime_ns, delta);
    }
    bool runnable = sched_runnable_count() != 0U;
    bool timeslice_expired =
        current->sched_class == SCHED_CLASS_RT &&
        current->sched.slice_runtime_ns >= SCHED_RT_TIMESLICE_NS;
    if (runnable &&
        ((current->sched_class == SCHED_CLASS_FAIR && delta != 0U) ||
         timeslice_expired)) {
        atomic_store_explicit(&cpu->need_resched, true,
                              memory_order_release);
    }
}

bool sched_fair_policy_self_test(void) {
    if (scheduler_fair_vruntime_delta(100U, SCHED_NICE_0_WEIGHT) != 100U ||
        scheduler_fair_vruntime_delta(100U, 2048U) != 50U ||
        scheduler_fair_vruntime_delta(100U, 512U) != 200U) {
        return false;
    }
    if (scheduler_fair_vruntime_delta(UINT64_MAX, 1U) != UINT64_MAX ||
        scheduler_vruntime_add(UINT64_MAX - 10U, 20U) != UINT64_MAX) {
        return false;
    }
    return true;
}

bool sched_tick_should_preempt(void) {
    uint32_t cpu_id;
    return scheduler_current_cpu(&cpu_id) &&
           atomic_load_explicit(&g_cpus[cpu_id].need_resched,
                                memory_order_acquire);
}
