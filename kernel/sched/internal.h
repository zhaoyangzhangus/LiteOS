#pragma once

#include <kernel/sched.h>

#define SCHED_ENTITY_ENQUEUED             (1U << 0)
#define SCHED_ENTITY_INITIAL_PLACEMENT   (1U << 1)
#define SCHED_ENTITY_REAP_QUEUED         (1U << 2)

/* Private scheduler state shared only by the scheduler implementation files. */
typedef struct scheduler_cpu {
    run_queue_t queue;

    /* Threads that left their execution stack and await final reclamation. */
    thread_t *reap_head;
    thread_t *reap_tail;

    /* At most one deferred reaper drain may be queued per CPU. */
    atomic_bool reap_deferred_queued;

    vaddr_t idle_stack_top;
} scheduler_cpu_t;

extern scheduler_cpu_t g_cpus[MAX_CPUS];
extern uint32_t g_cpu_count;

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
