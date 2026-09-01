#include "internal.h"

/*
 * LAPIC clock accounting and timeslice expiry live in this translation unit.
 * The call remains direct: the timer path still invokes sched_tick() without
 * a scheduler object or runtime dispatch layer.
 */
void sched_tick(uint64_t now_ns) {
    uint32_t cpu_id;
    if (!scheduler_current_cpu(&cpu_id)) return;
    scheduler_cpu_t *cpu = &g_cpus[cpu_id];
    cpu->queue.clock_ns = now_ns;
    thread_t *current = cpu->queue.current;
    if (current == 0) return;
    if (current == cpu->queue.idle) {
        if (sched_runnable_count() != 0) schedule();
        return;
    }
    uint64_t delta = current->sched.exec_start_ns == 0 ?
                     0 : now_ns - current->sched.exec_start_ns;
    current->sched.runtime_ns += delta;
    current->sched.exec_start_ns = now_ns;
    if (current->sched_class == SCHED_CLASS_FAIR) {
        current->sched.vruntime += delta;
    }
    if (current->sched_class == SCHED_CLASS_RT || delta != 0) schedule();
}
