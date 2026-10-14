#include <arch/x86_64/context.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/smp.h>
#include <kernel/deferred.h>
#include <kernel/process.h>
#include <kernel/rcu.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>

#define DEFERRED_QUEUE_CAPACITY 128U
#define DEFERRED_CRITICAL_BURST_LIMIT 4U
#define DEFERRED_WORKER_STACK_SIZE (64U * 1024U)

typedef struct deferred_item {
    deferred_work_fn_t function;
    void *argument;
} deferred_item_t;

static struct {
    spinlock_t lock;
    deferred_item_t items[DEFERRED_QUEUE_CAPACITY];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    /* Protected by lock; persists across short batches and all consumers. */
    uint32_t critical_burst;
    deferred_item_t critical_item;
    bool critical_ready; /* protected by lock, including the sleep handshake */
    atomic_uint init_state;

    /*
     * One global worker matches the current one-global-queue architecture.
     * It is a persistent Ring0 scheduler thread: no process, no user CR3.
     */
    thread_t worker;
    atomic_bool worker_started;
} g_deferred;

static volatile uint32_t g_deferred_progress[MAX_CPUS];
static volatile uint32_t g_deferred_waiting[MAX_CPUS];
static volatile uint32_t g_deferred_lock_owner = UINT32_MAX;

static void deferred_debug_set(uint32_t value) {
    uint32_t cpu_index = x86_current_cpu_index();
    if (cpu_index < MAX_CPUS) {
        __atomic_store_n(&g_deferred_progress[cpu_index], value,
                         __ATOMIC_RELEASE);
    }
}

uint32_t deferred_debug_progress(uint32_t cpu_index) {
    if (cpu_index >= MAX_CPUS) return 0U;
    return __atomic_load_n(&g_deferred_progress[cpu_index], __ATOMIC_ACQUIRE);
}

uint32_t deferred_debug_waiting(uint32_t cpu_index) {
    if (cpu_index >= MAX_CPUS) return 0U;
    return __atomic_load_n(&g_deferred_waiting[cpu_index], __ATOMIC_ACQUIRE);
}

uint32_t deferred_debug_lock_owner(void) {
    return __atomic_load_n(&g_deferred_lock_owner, __ATOMIC_ACQUIRE);
}

uint32_t deferred_debug_lock_state(void) {
    return atomic_load_explicit(&g_deferred.lock.state, memory_order_acquire);
}

uint32_t deferred_debug_worker_state(void) {
    return atomic_load_explicit(&g_deferred.worker.state, memory_order_acquire);
}

uint32_t deferred_debug_queue_count(void) {
    return __atomic_load_n(&g_deferred.count, __ATOMIC_ACQUIRE);
}

static uint8_t g_deferred_worker_stack[DEFERRED_WORKER_STACK_SIZE]
    __attribute__((aligned(16)));

/*
 * Device IRQs enqueue work through deferred_try_schedule().  A plain
 * try-lock is not sufficient here: if an xHCI completion arrives while a
 * normal producer owns the queue lock, acknowledging the IRQ and returning
 * loses the only wakeup for that HID transfer.  The next report cannot be
 * queued until this event is consumed, so keyboard and mouse input stops
 * permanently.
 *
 * Keep the queue's critical sections IRQ-safe and tiny.  Once a CPU owns the
 * lock it cannot be interrupted locally; another CPU may spin only until the
 * fixed-size ring bookkeeping below completes.  This is event-driven, not a
 * timer poll or a retry loop for xHCI.
 */
static uint64_t deferred_irq_save(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static void deferred_irq_restore(uint64_t flags) {
    if ((flags & (1ULL << 9)) != 0U) {
        __asm__ volatile ("sti" : : : "memory");
    } else {
        __asm__ volatile ("cli" : : : "memory");
    }
}

static uint64_t deferred_lock(void) {
    uint64_t flags = deferred_irq_save();
    /*
     * Queue producers include normal syscall context as well as IRQ/deferred
     * context.  Interrupt masking protects the local critical section, but
     * it does not stop the scheduler from moving a normal producer to
     * another CPU while it owns the global queue lock.  Keep the owner on
     * this CPU until the ring bookkeeping is published.
     */
    uint32_t cpu_index = x86_current_cpu_index();
    sched_preempt_disable();
    if (cpu_index < MAX_CPUS) {
        __atomic_store_n(&g_deferred_waiting[cpu_index], 1U,
                         __ATOMIC_RELEASE);
    }
    spinlock_lock(&g_deferred.lock);
    __atomic_store_n(&g_deferred_lock_owner, cpu_index, __ATOMIC_RELEASE);
    if (cpu_index < MAX_CPUS) {
        __atomic_store_n(&g_deferred_waiting[cpu_index], 0U,
                         __ATOMIC_RELEASE);
    }
    return flags;
}

static void deferred_unlock(uint64_t flags) {
    __atomic_store_n(&g_deferred_lock_owner, UINT32_MAX, __ATOMIC_RELEASE);
    spinlock_unlock(&g_deferred.lock);
    sched_preempt_enable();
    deferred_irq_restore(flags);
}

static void deferred_wake_worker(void) {
    deferred_debug_set(7U);
    if (!atomic_load_explicit(&g_deferred.worker_started,
                              memory_order_acquire)) {
        return;
    }

    /*
     * sched_wake() is IRQ-safe.  If the worker is still RUNNING this is only
     * a hint; the queue lock handshake below guarantees it will observe the
     * newly queued item before it can block.
    */
    sched_wake(&g_deferred.worker);
    deferred_debug_set(8U);

    /*
     * sched_wake() is intentionally a state transition API.  If a previous
     * wake already left this worker READY in the run queue, there is no new
     * BLOCKED -> READY transition to trigger an IPI.  Retry the target CPU
     * request for that state as well; this closes the pending-IPI race where
     * the worker is queued but the earlier request was already consumed.
     */
    if (atomic_load_explicit(&g_deferred.worker.state,
                             memory_order_acquire) != THREAD_RUNNING) {
        deferred_debug_set(9U);
        (void)x86_smp_request_reschedule(g_deferred.worker.owner_cpu);
    }
    deferred_debug_set(10U);
}

static bool deferred_worker_has_work_locked(void) {
    return g_deferred.count != 0U || g_deferred.critical_ready;
}

static bool deferred_worker_prepare_sleep(void) {
    thread_t *thread = sched_current_thread();
    if (thread != &g_deferred.worker) return false;

    /*
     * Lost-wakeup barrier:
     *
     * Producer:
     *     deferred_lock -> enqueue/publish -> unlock -> sched_wake(worker)
     *
     * Consumer:
     *     deferred_lock -> verify empty -> RUNNING->BLOCKED -> unlock
     *                   -> schedule()
     *
     * Therefore an IRQ can never publish work in the gap between the empty
     * test and BLOCKED publication.  If it publishes after unlock,
     * sched_wake() sees BLOCKED (or READY/current) and preserves the wakeup.
     */
    uint64_t flags = deferred_lock();

    if (deferred_worker_has_work_locked()) {
        deferred_unlock(flags);
        return false;
    }

    bool blocked = sched_publish_blocked(thread);

    deferred_unlock(flags);
    return blocked;
}

static void __attribute__((noreturn))
deferred_worker_main(void *argument) {
    (void)argument;

    for (;;) {
        /*
         * A bounded batch prevents a permanently busy device from keeping
         * this FAIR kernel worker on-CPU forever.
         */
        uint32_t completed = deferred_run(64U);

        if (completed == 64U) {
            schedule();
            continue;
        }

        /*
         * deferred_run() may have stopped because the queue became empty.
         * Recheck under the producer lock and publish BLOCKED atomically with
         * that empty observation.
         */
        if (deferred_worker_prepare_sleep()) {
            schedule();
        } else {
            __asm__ volatile ("pause");
        }
    }
}

bool deferred_init(void) {
    /* After initial publication, queue operations use their own locks.
     * Observe readiness without rewriting the shared init cache line. */
    if (atomic_load_explicit(&g_deferred.init_state, memory_order_acquire) == 2U) return true;
    unsigned expected = 0U;
    if (atomic_compare_exchange_strong_explicit(&g_deferred.init_state, &expected, 1U,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        atomic_init(&g_deferred.lock.state, 0U);
        g_deferred.head = 0;
        g_deferred.tail = 0;
        g_deferred.count = 0;
        g_deferred.critical_burst = 0;
        g_deferred.critical_item.function = 0;
        g_deferred.critical_item.argument = 0;
        g_deferred.critical_ready = false;
        atomic_init(&g_deferred.worker_started, false);
        for (uint32_t cpu_index = 0U; cpu_index < MAX_CPUS; ++cpu_index) {
            __atomic_store_n(&g_deferred_progress[cpu_index], 0U,
                             __ATOMIC_RELAXED);
            __atomic_store_n(&g_deferred_waiting[cpu_index], 0U,
                             __ATOMIC_RELAXED);
        }
        __atomic_store_n(&g_deferred_lock_owner, UINT32_MAX,
                         __ATOMIC_RELAXED);
        atomic_store_explicit(&g_deferred.init_state, 2U, memory_order_release);
        return true;
    }
    while (atomic_load_explicit(&g_deferred.init_state, memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
    return true;
}

bool deferred_start_worker(void) {
    if (!deferred_init()) return false;

    if (atomic_load_explicit(&g_deferred.worker_started,
                             memory_order_acquire)) {
        return true;
    }

    /*
     * This is the explicit scheduler-ready boundary.  Early boot calls only
     * deferred_init()/deferred_run(); they never need a schedulable thread.
     */
    thread_t *current = sched_current_thread();
    uint32_t cpu_id = x86_current_cpu_index();
    if (current == 0 || cpu_id >= MAX_CPUS) return false;

    thread_t *worker = &g_deferred.worker;
    memset(worker, 0, sizeof(*worker));

    refcount_init(&worker->object.refs, 1U);
    worker->object.type = KOBJECT_TYPE_THREAD;
    worker->object.flags = 0U;
    worker->object.ops = 0;

    /*
     * TID 0 is reserved from normal process allocation (user TIDs begin at
     * 1) and gives the bottom-half worker deterministic first ordering when
     * FAIR vruntime ties at boot.
     */
    worker->tid = 0U;
    worker->process = 0;
    atomic_init(&worker->state, THREAD_READY);
    atomic_init(&worker->block_epoch, 0U);
    atomic_init(&worker->command_ack, 0U);

    worker->kernel_stack_base = g_deferred_worker_stack;
    worker->kernel_stack_size = sizeof(g_deferred_worker_stack);
    worker->kernel_stack_top =
        ((vaddr_t)(uintptr_t)g_deferred_worker_stack +
         sizeof(g_deferred_worker_stack)) &
        ~(vaddr_t)0x0FULL;

    worker->sched_class = SCHED_CLASS_FAIR;
    worker->base_sched_class = SCHED_CLASS_FAIR;
    worker->rt_priority = 0U;
    worker->base_rt_priority = 0U;
    worker->sched.weight = 1024U;
    worker->sched.nice = 0;
    worker->sched.vruntime = 0U;

    list_init(&worker->sched.rt_node);
    list_init(&worker->process_node);
    list_init(&worker->owned_mutexes);

    for (uint32_t word = 0U; word < MAX_CPUS / 64U; ++word) {
        worker->affinity.bits[word] = 0U;
    }
    worker->affinity.bits[cpu_id >> 6] =
        1ULL << (cpu_id & 63U);
    worker->owner_cpu = (uint16_t)cpu_id;
    worker->current_cpu = (uint16_t)cpu_id;

    /*
     * x86_switch_context_root() finishes with RET.
     *
     * saved RSP points at the synthetic return address.  After RET the
     * trampoline sees a 16-byte-aligned RSP; its CALL then enters C with the
     * SysV-required RSP % 16 == 8.
     */
    uintptr_t stack_top = (uintptr_t)worker->kernel_stack_top;
    uintptr_t switch_stack = stack_top - sizeof(uint64_t);
    *(uint64_t *)switch_stack =
        (uint64_t)(uintptr_t)&x86_kernel_thread_start;

    worker->arch.switch_ctx.rsp = switch_stack;
    worker->arch.switch_ctx.r12 =
        (uint64_t)(uintptr_t)&deferred_worker_main;
    worker->arch.switch_ctx.r13 = 0U;
    worker->arch.switch_ctx.r14 = stack_top;
    worker->arch.fs_base = 0U;

    /*
     * Publish the wake target before making it runnable.  A concurrent IRQ
     * may call sched_wake() while state is READY; that is harmless because
     * sched_enqueue() below already preserves the runnable instance.
     */
    atomic_store_explicit(&g_deferred.worker_started, true,
                          memory_order_release);

    bool enqueued = sched_enqueue_bootstrap(worker);
    if (!enqueued) {
        atomic_store_explicit(&g_deferred.worker_started, false,
                              memory_order_release);
        return false;
    }

    /*
     * Do not synchronously switch from the user-init bootstrap continuation.
     * The worker is already READY and queued; its reschedule request and the
     * next normal scheduling boundary will run it without stranding the
     * bootstrap thread inside a first-run context hand-off.
     */

    return true;
}

bool deferred_worker_started(void) {
    return atomic_load_explicit(&g_deferred.worker_started,
                                memory_order_acquire);
}

/* Caller holds the IRQ-safe queue lock. Keep both producer entry points on
 * one ring publication path, with their existing diagnostics and wakeups. */
static bool deferred_enqueue_locked(deferred_work_fn_t function, void *argument) {
    if (g_deferred.count == DEFERRED_QUEUE_CAPACITY) return false;
    g_deferred.items[g_deferred.tail].function = function;
    g_deferred.items[g_deferred.tail].argument = argument;
    g_deferred.tail = (g_deferred.tail + 1U) % DEFERRED_QUEUE_CAPACITY;
    ++g_deferred.count;
    return true;
}

bool deferred_schedule(deferred_work_fn_t function, void *argument) {
    uint64_t flags;
    deferred_debug_set(1U);
    if (function == 0 || !deferred_init()) return false;
    deferred_debug_set(2U);
    flags = deferred_lock();
    deferred_debug_set(4U);
    if (!deferred_enqueue_locked(function, argument)) {
        deferred_unlock(flags);
        return false;
    }
    deferred_debug_set(5U);
    deferred_unlock(flags);
    deferred_debug_set(6U);
    deferred_wake_worker();
    deferred_debug_set(11U);
    return true;
}

bool deferred_try_schedule(deferred_work_fn_t function, void *argument) {
    uint64_t flags;
    if (function == 0 || !deferred_init()) return false;
    flags = deferred_lock();
    if (!deferred_enqueue_locked(function, argument)) {
        deferred_unlock(flags);
        return false;
    }
    deferred_unlock(flags);
    deferred_wake_worker();
    return true;
}

bool deferred_schedule_critical(deferred_work_fn_t function, void *argument) {
    uint64_t flags;

    /*
     * This single slot is deliberately reserved for the xHCI MSI-X worker.
     * Its caller owns a coalescing queued bit covering pending and executing
     * device work. Do not turn this into a shared fallback queue without
     * adding per-source ownership.
     *
     * Publication now shares deferred_lock with the worker's sleep handshake.
     * That is what closes the EMPTY->BLOCKED lost-wakeup window for the
     * emergency slot as well as the normal ring.
     */
    if (function == 0 || !deferred_init()) return false;

    flags = deferred_lock();

    if (g_deferred.critical_ready) {
        deferred_unlock(flags);
        return false;
    }

    g_deferred.critical_item.function = function;
    g_deferred.critical_item.argument = argument;

    g_deferred.critical_ready = true;

    deferred_unlock(flags);
    deferred_wake_worker();
    return true;
}

/* Caller holds the queue lock. Empty the slot before callback execution,
 * preserving the existing ability to enqueue follow-up work from a callback. */
static bool deferred_take_critical(deferred_item_t *item) {
    if (item == 0 || !g_deferred.critical_ready) {
        return false;
    }
    *item = g_deferred.critical_item;
    g_deferred.critical_ready = false;
    return item->function != 0;
}

uint32_t deferred_run(uint32_t budget) {
    uint32_t completed = 0;
    if (budget == 0U || !deferred_init()) return 0;
    while (completed < budget) {
        deferred_item_t item = {0};
        /* Serialize selection, not execution. A shared burst count prevents
         * short syscall batches (and concurrent consumers) from resetting
         * HID priority before a waiting network task gets its turn. */
        uint64_t flags = deferred_lock();
        bool have_item = false;
        if (g_deferred.critical_burst < DEFERRED_CRITICAL_BURST_LIMIT ||
            g_deferred.count == 0U) {
            have_item = deferred_take_critical(&item);
        }
        if (have_item) {
            if (g_deferred.critical_burst < DEFERRED_CRITICAL_BURST_LIMIT)
                ++g_deferred.critical_burst;
        } else if (g_deferred.count != 0U) {
            item = g_deferred.items[g_deferred.head];
            g_deferred.head = (g_deferred.head + 1U) % DEFERRED_QUEUE_CAPACITY;
            --g_deferred.count;
            have_item = item.function != 0;
            g_deferred.critical_burst = 0;
        } else {
            g_deferred.critical_burst = 0;
        }
        deferred_unlock(flags);
        if (!have_item) break;
        item.function(item.argument);
        ++completed;
    }
    /* RCU 回调在可抢占的普通内核上下文中执行，不能放进硬中断。 */
    if (completed < budget) (void)rcu_poll(budget - completed);
    return completed;
}

static void deferred_self_test_work(void *argument) {
    atomic_uint *counter = (atomic_uint *)argument;
    if (counter != 0) atomic_fetch_add_explicit(counter, 1U, memory_order_relaxed);
}

bool deferred_self_test(void) {
    atomic_uint counter;
    atomic_init(&counter, 0U);
    if (!deferred_schedule(deferred_self_test_work, &counter) ||
        !deferred_schedule(deferred_self_test_work, &counter) ||
        deferred_run(2U) != 2U) return false;
    return atomic_load_explicit(&counter, memory_order_relaxed) == 2U;
}
