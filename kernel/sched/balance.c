#include <arch/x86_64/smp.h>
#include "internal.h"

bool sched_balance_self_test(void) {
    thread_t thread;
    uint32_t current_cpu;
    if (!scheduler_current_cpu(&current_cpu) || g_cpu_count == 0U) return false;

    initialize_thread(&thread, UINT64_C(0x42414c414e4345),
                      SCHED_CLASS_FAIR, 0U);
    thread.owner_cpu = (uint16_t)current_cpu;
    thread.current_cpu = (uint16_t)current_cpu;
    uint32_t selected = scheduler_choose_cpu(&thread, current_cpu);
    if (!scheduler_cpu_available(selected)) return false;

    /* Initial starts must honor affinity instead of blindly using the
     * creating CPU. */
    uint32_t remote_cpu = UINT32_MAX;
    for (uint32_t cpu_id = 0U; cpu_id < g_cpu_count; ++cpu_id) {
        if (cpu_id != current_cpu && scheduler_cpu_available(cpu_id)) {
            remote_cpu = cpu_id;
            break;
        }
    }
    if (remote_cpu != UINT32_MAX) {
        thread_t initial_thread;
        initialize_thread(&initial_thread, UINT64_C(0x5354415254504c43),
                          SCHED_CLASS_FAIR, 0U);
        initial_thread.owner_cpu = (uint16_t)current_cpu;
        initial_thread.current_cpu = (uint16_t)current_cpu;
        initial_thread.sched.flags |= SCHED_ENTITY_INITIAL_PLACEMENT;
        for (uint32_t word = 0U; word < MAX_CPUS / 64U; ++word) {
            initial_thread.affinity.bits[word] = 0U;
        }
        initial_thread.affinity.bits[remote_cpu >> 6] =
            1ULL << (remote_cpu & 63U);
        if (scheduler_choose_start_cpu(&initial_thread, current_cpu) !=
            remote_cpu) {
            return false;
        }
    }

    /* A restricted mask must override the load-based choice. */
    for (uint32_t word = 0; word < MAX_CPUS / 64U; ++word) {
        thread.affinity.bits[word] = 0U;
    }
    thread.affinity.bits[current_cpu >> 6] = 1ULL << (current_cpu & 63U);
    cpumask_t empty_mask = {0};
    uint32_t no_selection = UINT32_MAX;
    if (scheduler_select_cpu_mask(&empty_mask, current_cpu, &no_selection) ||
        no_selection != UINT32_MAX) return false;
    if (sched_set_affinity(&thread, &empty_mask) != K_EINVAL) return false;
    if (g_cpu_count < MAX_CPUS) {
        cpumask_t undiscovered_mask = {0};
        undiscovered_mask.bits[g_cpu_count >> 6] = 1ULL << (g_cpu_count & 63U);
        if (sched_set_affinity(&thread, &undiscovered_mask) != K_EINVAL)
            return false;
    }
    for (uint32_t word = 0U; word < MAX_CPUS / 64U; ++word) {
        uint64_t expected = word == (current_cpu >> 6) ?
                            1ULL << (current_cpu & 63U) : 0U;
        if (thread.affinity.bits[word] != expected) return false;
    }
    return scheduler_choose_cpu(&thread, current_cpu) == current_cpu;
}

bool scheduler_cpu_available(uint32_t cpu_id) {
    return cpu_id < g_cpu_count && x86_smp_cpu_online(cpu_id) &&
           g_cpus[cpu_id].queue.idle != 0;
}

bool scheduler_select_cpu_mask(const cpumask_t *mask, uint32_t current_cpu,
                               uint32_t *selected_cpu) {
    if (mask == 0 || selected_cpu == 0) return false;
    uint32_t best_cpu = current_cpu;
    uint32_t best_load = UINT32_MAX;
    bool found = false;
    uint32_t cpu_count = g_cpu_count < MAX_CPUS ? g_cpu_count : MAX_CPUS;
    for (uint32_t base = 0U; base < cpu_count; base += 64U) {
        uint64_t candidates = mask->bits[base >> 6];
        uint32_t remaining = cpu_count - base;
        if (remaining < 64U) candidates &= (1ULL << remaining) - 1ULL;
        /* Ascending set-bit order retains the existing tie-break rules while
         * skipping CPUs the thread cannot use. Never scan zero with ctz. */
        while (candidates != 0U) {
            uint32_t cpu_id = base + (uint32_t)__builtin_ctzll(candidates);
            candidates &= candidates - 1U;
            if (!scheduler_cpu_available(cpu_id)) continue;
            scheduler_cpu_t *cpu = &g_cpus[cpu_id];
            uint32_t load = scheduler_snapshot_load(cpu);
            if (!found || load < best_load ||
                (load == best_load && cpu_id == current_cpu)) {
                best_cpu = cpu_id;
                best_load = load;
                found = true;
            }
        }
    }
    if (found) *selected_cpu = best_cpu;
    return found;
}

uint32_t scheduler_choose_cpu(thread_t *thread, uint32_t current_cpu) {
    uint32_t selected_cpu;
    return scheduler_select_cpu_mask(thread != 0 ? &thread->affinity : 0,
                                     current_cpu, &selected_cpu) ?
           selected_cpu : current_cpu;
}

uint32_t scheduler_choose_start_cpu(thread_t *thread, uint32_t current_cpu) {
    if (thread == 0) return current_cpu;
    if ((thread->sched.flags & SCHED_ENTITY_INITIAL_PLACEMENT) != 0U) {
        return scheduler_choose_cpu(thread, current_cpu);
    }
    return scheduler_cpu_available(thread->owner_cpu) ? thread->owner_cpu :
                                                        current_cpu;
}
