#pragma once
#pragma once
#include "base.h"
#include "object.h"
#include "list.h"
#include "rbtree.h"
#include "spinlock.h"
#include "cpumask.h"
#include <arch/x86_64/context.h>

struct process;
struct waiter;

enum thread_state {
    THREAD_NEW = 0,
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_BLOCKED,
    THREAD_STOPPED,
    THREAD_DEAD,
};

enum sched_class_id {
    SCHED_CLASS_RT = 0,
    SCHED_CLASS_FAIR,
    SCHED_CLASS_IDLE,
};

typedef struct sched_entity {
    uint64_t runtime_ns;
    uint64_t vruntime;
    uint64_t exec_start_ns;
    uint64_t slice_runtime_ns;
    uint32_t weight;
    int8_t nice;
    int8_t latency_hint;
    uint16_t flags;
    rb_node_t fair_node;
    list_head_t rt_node;
} sched_entity_t;

typedef struct thread {
    /* Process-owned object identity and lifecycle association. */
    object_header_t object;
    uint64_t tid;
    struct process *process;
    /* Scheduler-owned runnable-state publication; Process only observes it. */
    atomic_uint state;
    /*
     * Scheduler-owned blocking generation.
     * The owner CPU increments it before publishing THREAD_BLOCKED.
     * Remote SPSC wake commands carry this value to reject stale wakes.
     */
     atomic_uint block_epoch;
    /* Non-owner start requests are serialized independently of state. */
    atomic_bool start_pending;
    /* Process-owned execution-reference and reaping lifetime flags. */
    uint32_t flags;

    /* Architecture-owned execution stack and saved CPU state. */
    void *kernel_stack_base;
    vaddr_t kernel_stack_top;
    size_t kernel_stack_size;
    arch_thread_state_t arch;

    /* Optional user stack owned by a pthread-style creator.  The scheduler
     * reaper clears this mapping after the final context switch. */
    vaddr_t user_stack_base;
    size_t user_stack_size;
    bool user_stack_owned;

    /* Scheduler-owned accounting and queue membership. */
    sched_entity_t sched;
    cpumask_t affinity;
    /* Runqueue owner; only the owner may mutate queue linkage. */
    uint16_t owner_cpu;
    /* Current/last execution placement, kept separate from owner_cpu. */
    uint16_t current_cpu;
    /* Owner-local migration hand-off for a currently running thread. */
    uint16_t migration_target_cpu;
    bool migration_pending;
    uint8_t sched_class;
    uint8_t rt_priority;
    uint8_t base_sched_class;
    uint8_t base_rt_priority;

    /* Wait/synchronization-owned PI state. */
    list_head_t owned_mutexes;
    void *pi_blocked_on;

    /* Process-owned thread membership. */
    list_head_t process_node;

    /* Scheduler-owned post-context-switch reaper FIFO link. */
    struct thread *reap_next;
    /* Scheduler-owned coalesced command-reference release link. */
    struct thread *command_release_next;
    uint32_t command_release_count;
    /* Remote state/remove commands may wait for owner-side consumption. */
    atomic_uint command_ack;

    /* Wait-owned blocking association. */
    struct waiter * _Atomic blocked_waiter;

    /* Process-owned exit and exec publication state. */
    int64_t exit_code;
    bool exec_pending;
    vaddr_t exec_entry;
    vaddr_t exec_stack;

    uint64_t signal_pending;
    uint64_t signal_mask;
    vaddr_t signal_frame;
    uint32_t signal_depth;
} thread_t;

#define RT_PRIORITY_LEVELS 32u

typedef struct run_queue {
    thread_t *current;
    thread_t *idle;

    rb_root_t fair_root;
    uint64_t min_vruntime;
    uint32_t fair_count;

    list_head_t rt_queues[RT_PRIORITY_LEVELS];
    uint32_t rt_bitmap;
    uint32_t nr_running;

    uint64_t clock_ns;
} run_queue_t;

void sched_init(void);
void sched_cpu_init(uint32_t cpu_id);
/* Scheduler-owned initialization for a Process-created, not-yet-runnable thread. */
void sched_initialize_new_thread(thread_t *thread, uint32_t cpu_id,
                                 bool pin_to_cpu);
/* Scheduler-owned setup for stack-based synchronization self-test fixtures. */
void sched_initialize_test_thread(thread_t *thread, uint64_t tid,
                                  uint8_t class_id, uint8_t priority);
/* Publish scheduler-owned first-placement metadata before thread start. */
void sched_mark_initial_placement(thread_t *thread);
/* Returns true when a new runnable queue entry was published. */
bool sched_enqueue(thread_t *thread);
/*
 * Boot-only enqueue for the BSP bootstrap continuation.  It still sends a
 * remote reschedule request, but never self-IPs the logical idle entry whose
 * stack is the live boot continuation rather than a saved scheduler context.
 */
bool sched_enqueue_bootstrap(thread_t *thread);
kstatus_t sched_start_thread(thread_t *thread);
bool sched_publish_blocked(thread_t *thread);
bool sched_publish_running(thread_t *thread);
/* Scheduler only publishes the terminal runnable-state transition. */
bool sched_publish_dead(thread_t *thread);
void sched_wake(thread_t *thread);
void sched_block_current(void);
void schedule(void);
/* Consume an IPI doorbell and schedule only when the local policy requires it. */
void sched_handle_reschedule_request(void);
void sched_tick(uint64_t now_ns);
bool sched_tick_should_preempt(void);
kstatus_t sched_set_affinity(thread_t *thread, const cpumask_t *mask);
void sched_set_effective_priority(thread_t *thread, uint8_t class_id,
                                  uint8_t rt_priority);

thread_t *sched_current_thread(void);
void sched_remove(thread_t *thread);
void sched_finish_switch(void);
uint32_t sched_runnable_count(void);
bool sched_try_run_ready(void);
void sched_preempt_disable(void);
void sched_preempt_enable(void);
bool sched_preempt_disabled(void);
bool sched_validate_current_cpu(void);
bool sched_accounting_self_test(void);
bool sched_fair_policy_self_test(void);
bool sched_rt_policy_self_test(void);
bool sched_balance_self_test(void);
bool sched_state_transition_self_test(void);
bool sched_remote_wake_self_test(void);
bool sched_context_switch_self_test(void);
bool sched_debug_cpu(uint32_t cpu_id, uint32_t *current_state,
                     uint64_t *current_tid, uint32_t *runnable_count);
bool sched_cpu_uses_root(uint32_t cpu_id, paddr_t root);
/* 引导栈切换到高地址别名后，同步空闲线程所使用的 Ring0 栈顶。 */
bool sched_set_boot_kernel_stack(vaddr_t stack_top);

/* 启动阶段诊断：返回最近一次入队的线程，正式版本由 trace 设施替代。 */
