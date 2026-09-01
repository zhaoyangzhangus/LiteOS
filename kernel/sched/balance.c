#include <arch/x86_64/smp.h>
#include "internal.h"

bool sched_balance_self_test(void) {
    thread_t thread;
    uint32_t current_cpu;
    if (!scheduler_current_cpu(&current_cpu) || g_cpu_count == 0U) return false;

    initialize_thread(&thread, UINT64_C(0x42414c414e4345),
                      SCHED_CLASS_FAIR, 0U);
    thread.current_cpu = (uint16_t)current_cpu;
    uint32_t selected = scheduler_choose_cpu(&thread, current_cpu);
    if (!scheduler_cpu_available(selected)) return false;

    /* A restricted mask must override the load-based choice. */
    for (uint32_t word = 0; word < MAX_CPUS / 64U; ++word) {
        thread.affinity.bits[word] = 0U;
    }
    thread.affinity.bits[current_cpu >> 6] = 1ULL << (current_cpu & 63U);
    return scheduler_choose_cpu(&thread, current_cpu) == current_cpu;
}

bool scheduler_cpu_available(uint32_t cpu_id) {
    return cpu_id < g_cpu_count && x86_smp_cpu_online(cpu_id) &&
           g_cpus[cpu_id].queue.idle != 0;
}

static bool scheduler_affinity_allows(const thread_t *thread, uint32_t cpu_id) {
    return thread != 0 && cpu_id < MAX_CPUS &&
           (thread->affinity.bits[cpu_id >> 6] & (1ULL << (cpu_id & 63U))) != 0;
}

uint32_t scheduler_choose_cpu(thread_t *thread, uint32_t current_cpu) {
    uint32_t best_cpu = current_cpu;
    uint32_t best_load = UINT32_MAX;
    for (uint32_t cpu_id = 0; cpu_id < g_cpu_count; ++cpu_id) {
        if (!scheduler_cpu_available(cpu_id) ||
            !scheduler_affinity_allows(thread, cpu_id)) continue;
        scheduler_cpu_t *cpu = &g_cpus[cpu_id];
        uint64_t flags = scheduler_lock(&cpu->queue.lock);
        uint32_t load = cpu->queue.nr_running +
                        (cpu->queue.current != cpu->queue.idle ? 1U : 0U);
        scheduler_unlock(&cpu->queue.lock, flags);
        if (load < best_load || (load == best_load && cpu_id == current_cpu)) {
            best_cpu = cpu_id;
            best_load = load;
        }
    }
    return best_cpu;
}
