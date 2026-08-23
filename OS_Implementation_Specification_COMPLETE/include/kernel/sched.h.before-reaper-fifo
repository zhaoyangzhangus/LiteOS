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
    uint32_t weight;
    int8_t nice;
    int8_t latency_hint;
    uint16_t flags;
    rb_node_t fair_node;
    list_head_t rt_node;
} sched_entity_t;

typedef struct thread {
    object_header_t object;
    uint64_t tid;

    struct process *process;
    atomic_uint state;
    uint32_t flags;

    void *kernel_stack_base;
    vaddr_t kernel_stack_top;
    size_t kernel_stack_size;

    arch_thread_state_t arch;
    sched_entity_t sched;

    cpumask_t affinity;
    uint16_t current_cpu;
    uint8_t sched_class;
    uint8_t rt_priority;
    uint8_t base_sched_class;
    uint8_t base_rt_priority;

    /* PI 互斥锁使用；owned_mutexes 中保存当前线程持有的锁。 */
    list_head_t owned_mutexes;
    void *pi_blocked_on;

    list_head_t process_node;
    list_head_t global_node;

    struct waiter *blocked_waiter;
    int64_t exit_code;

    /* PROCESS_EXEC 成功后由系统调用返回路径消费的新用户入口。 */
    bool exec_pending;
    vaddr_t exec_entry;
    vaddr_t exec_stack;
} thread_t;

#define RT_PRIORITY_LEVELS 32u

typedef struct run_queue {
    spinlock_t lock;
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
void sched_enqueue(thread_t *thread);
void sched_wake(thread_t *thread);
void sched_block_current(void);
void schedule(void);
void sched_tick(uint64_t now_ns);
kstatus_t sched_set_affinity(thread_t *thread, const cpumask_t *mask);
void sched_set_effective_priority(thread_t *thread, uint8_t class_id,
                                  uint8_t rt_priority);
