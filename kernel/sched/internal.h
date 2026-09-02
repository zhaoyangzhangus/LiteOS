#pragma once

#include <kernel/sched.h>

#define SCHED_ENTITY_ENQUEUED             (1U << 0)
#define SCHED_ENTITY_INITIAL_PLACEMENT   (1U << 1)
#define SCHED_ENTITY_REAP_QUEUED         (1U << 2)

#define SCHED_CMD_SLOTS 8U
#define SCHED_CMD_MASK  (SCHED_CMD_SLOTS - 1U)
#define SCHED_CMD_OVERFLOW_SLOTS 64U
#define SCHED_CMD_OVERFLOW_MASK  (SCHED_CMD_OVERFLOW_SLOTS - 1U)

#define SCHED_CMD_WAKE    1U
#define SCHED_CMD_ENQUEUE 2U
#define SCHED_CMD_SET_PRIORITY 3U
#define SCHED_CMD_SET_CLASS    4U
#define SCHED_CMD_REMOVE       5U
#define SCHED_CMD_STOP         6U
#define SCHED_CMD_EXIT         7U
#define SCHED_CMD_MIGRATE      8U
#define SCHED_CMD_START        9U

#define SCHED_CMD_CLASS_SHIFT 8U
#define SCHED_CMD_REMOVE_ACK   1U
#define SCHED_CMD_STATE_ACK    2U
#define SCHED_CMD_REMOVE_NO_REF (1U << 31)
#define SCHED_NICE_0_WEIGHT 1024U
#define SCHED_RT_TIMESLICE_NS 4000000ULL

_Static_assert((SCHED_CMD_SLOTS & (SCHED_CMD_SLOTS - 1U)) == 0U,
               "scheduler command ring size must be a power of two");
_Static_assert((SCHED_CMD_OVERFLOW_SLOTS &
                (SCHED_CMD_OVERFLOW_SLOTS - 1U)) == 0U,
               "scheduler overflow ring size must be a power of two");
_Static_assert(CACHELINE_SIZE > sizeof(atomic_uint),
               "cache line must be larger than atomic index");

typedef struct sched_cmd {
    uintptr_t object;
    uint32_t op;
    uint32_t generation;
} sched_cmd_t;

typedef struct sched_cmd_ring {
    /* Consumer-owned; producer only reads it. */
    atomic_uint head;
    uint8_t head_pad[CACHELINE_SIZE - sizeof(atomic_uint)];

    /* Producer-owned; consumer only reads it. */
    atomic_uint tail;
    uint8_t tail_pad[CACHELINE_SIZE - sizeof(atomic_uint)];

    sched_cmd_t slots[SCHED_CMD_SLOTS];
} __aligned(CACHELINE_SIZE) sched_cmd_ring_t;

typedef struct sched_cmd_overflow_ring {
    atomic_uint head;
    uint8_t head_pad[CACHELINE_SIZE - sizeof(atomic_uint)];
    atomic_uint tail;
    uint8_t tail_pad[CACHELINE_SIZE - sizeof(atomic_uint)];
    sched_cmd_t slots[SCHED_CMD_OVERFLOW_SLOTS];
} __aligned(CACHELINE_SIZE) sched_cmd_overflow_ring_t;

typedef struct sched_cmd_channel {
    sched_cmd_ring_t primary;
    sched_cmd_overflow_ring_t overflow;
    /* Producer-owned mode bit; the consumer only observes ring indices. */
    bool overflow_active;
    uint8_t producer_pad[CACHELINE_SIZE - sizeof(bool)];
} __aligned(CACHELINE_SIZE) sched_cmd_channel_t;

/* Private scheduler state shared only by the scheduler implementation files. */
typedef struct scheduler_cpu {
    run_queue_t queue;

    /*
     * Read-mostly scheduler load snapshot.
     * low 32 bits: queued runnable threads.
     * high 32 bits: queued + current non-idle runnable thread.
     */
    atomic_uint_fast64_t queue_snapshot;
    /* Timer accounting publishes preemption; the interrupt boundary consumes it. */
    atomic_bool preempt_pending;
    /*
     * Array[MAX_CPUS] of incoming SPSC command channels.
     * Ring N has producer CPU N and this scheduler CPU as consumer.
     * The overflow ring preserves ordering when the primary ring is full.
     */
    sched_cmd_channel_t *command_inbox;
    /* Producer-local count of commands that entered the overflow ring. */
    uint64_t command_ring_full_count;

    /* Threads that left their execution stack and await final reclamation. */
    thread_t *reap_head;
    thread_t *reap_tail;
    /* Remote command references released outside the owner IRQ boundary. */
    thread_t *command_release_head;
    thread_t *command_release_tail;
    /* Reaper may run from deferred context after the owner switched away. */
    spinlock_t reap_lock;

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
void enqueue_local(scheduler_cpu_t *cpu, thread_t *thread);
void dequeue_local(scheduler_cpu_t *cpu, thread_t *thread);
thread_t *pick_next_local(scheduler_cpu_t *cpu);
bool validate_fair_node(rb_node_t *node, rb_node_t *parent,
                        thread_t **previous, uint32_t black_depth,
                        uint32_t *expected_black_depth, uint32_t *count);
