#pragma once

#include <kernel/sched.h>

#define SCHED_ENTITY_ENQUEUED             (1U << 0)
#define SCHED_ENTITY_INITIAL_PLACEMENT   (1U << 1)
#define SCHED_ENTITY_REAP_QUEUED         (1U << 2)

/* Private scheduler state shared only by the scheduler implementation files. */
typedef struct scheduler_cpu {
    run_queue_t queue;

    /*
     * Read-mostly scheduler load snapshot.
     * low 32 bits: queued runnable threads.
     * high 32 bits: queued + current non-idle runnable thread.
     */
    atomic_uint_fast64_t queue_snapshot;

    /* Threads that left their execution stack and await final reclamation. */
    thread_t *reap_head;
    thread_t *reap_tail;

    /* At most one deferred reaper drain may be queued per CPU. */
    atomic_bool reap_deferred_queued;

    vaddr_t idle_stack_top;
} scheduler_cpu_t;

extern scheduler_cpu_t g_cpus[MAX_CPUS];
extern uint32_t g_cpu_count;

static inline uint64_t scheduler_make_queue_snapshot(
    const scheduler_cpu_t *cpu) {
    uint32_t runnable = cpu->queue.nr_running;
    uint32_t load = runnable;
    thread_t *current = cpu->queue.current;

    if (current != 0 &&
        current != cpu->queue.idle &&
        (current->sched.flags & SCHED_ENTITY_ENQUEUED) == 0U) {
        unsigned state =
            atomic_load_explicit(&current->state, memory_order_relaxed);
        if (state == THREAD_RUNNING || state == THREAD_READY) {
            ++load;
        }
    }

    return ((uint64_t)load << 32) | (uint64_t)runnable;
}

static inline void scheduler_publish_queue_snapshot(scheduler_cpu_t *cpu) {
    atomic_store_explicit(&cpu->queue_snapshot,
                          scheduler_make_queue_snapshot(cpu),
                          memory_order_release);
}

static inline uint32_t scheduler_snapshot_runnable(
    const scheduler_cpu_t *cpu) {
    return (uint32_t)atomic_load_explicit(&cpu->queue_snapshot,
                                          memory_order_acquire);
}

static inline uint32_t scheduler_snapshot_load(
    const scheduler_cpu_t *cpu) {
    return (uint32_t)(atomic_load_explicit(&cpu->queue_snapshot,
                                           memory_order_acquire) >> 32);
}

bool scheduler_current_cpu(uint32_t *cpu_id);
uint64_t scheduler_lock(spinlock_t *lock);
void scheduler_unlock(spinlock_t *lock, uint64_t flags);
bool scheduler_cpu_available(uint32_t cpu_id);
uint32_t scheduler_choose_cpu(thread_t *thread, uint32_t current_cpu);

void initialize_thread(thread_t *thread, uint64_t tid, uint8_t class_id,
                       uint8_t priority);
void enqueue_locked(scheduler_cpu_t *cpu, thread_t *thread);
void dequeue_locked(scheduler_cpu_t *cpu, thread_t *thread);
thread_t *pick_locked(scheduler_cpu_t *cpu);
bool validate_fair_node(rb_node_t *node, rb_node_t *parent,
                        thread_t **previous, uint32_t black_depth,
                        uint32_t *expected_black_depth, uint32_t *count);
