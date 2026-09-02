#include <kernel/sched.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/paging.h>
#include <arch/x86_64/smp.h>
#include <kernel/process.h>
#include <kernel/perf.h>
#include <kernel/telemetry.h>
#include <kernel/wait.h>
#include <kernel/deferred.h>
#include "internal.h"
#include <arch/x86_64/syscall_internal.h>

/* Definition shared with clock.c through the private scheduler header. */
scheduler_cpu_t g_cpus[MAX_CPUS];
static thread_t g_idle_threads[MAX_CPUS];
uint32_t g_cpu_count;
static paddr_t g_kernel_root;
static arch_switch_context_t *g_context_test_from;
static arch_switch_context_t *g_context_test_to;
static volatile uint32_t g_context_test_ran;

void sched_mark_initial_placement(thread_t *thread) {
    if (thread != 0) {
        thread->sched.flags |= SCHED_ENTITY_INITIAL_PLACEMENT;
    }
}

/*
 * Idle is a real scheduler target, but it is not created through the normal
 * thread creation path and therefore has no saved context at boot.  Seed a
 * synthetic context so a blocked thread can always switch to an interruptible
 * idle loop, even before the first live idle->thread transition captures the
 * AP/BSP startup loop.
 */
static __attribute__((noreturn)) void scheduler_idle_main(void *argument) {
    (void)argument;
    for (;;) {
        /*
         * A request can be published in the short CLI window between two
         * HLT cycles. Poll before sleeping so a lost IPI cannot strand a
         * READY thread on this idle CPU.
         */
        if (x86_smp_take_reschedule_request() ||
            sched_runnable_count() != 0U) {
            (void)sched_try_run_ready();
        }
        __asm__ volatile ("sti; hlt" : : : "memory");
        sched_finish_switch();
        if (x86_smp_take_reschedule_request() ||
            sched_runnable_count() != 0U) {
            (void)sched_try_run_ready();
        }
        __asm__ volatile ("cli" : : : "memory");
    }
}

static void scheduler_init_idle_context(thread_t *idle,
                                        vaddr_t stack_top) {
    uintptr_t switch_stack;
    if (idle == 0 || stack_top < sizeof(uint64_t) ||
        (stack_top & 0x0FU) != 0U) return;
    switch_stack = (uintptr_t)stack_top - sizeof(uint64_t);
    *(uint64_t *)switch_stack = (uint64_t)(uintptr_t)&x86_kernel_thread_start;
    idle->arch.switch_ctx.rsp = switch_stack;
    idle->arch.switch_ctx.rbx = 0U;
    idle->arch.switch_ctx.rbp = 0U;
    idle->arch.switch_ctx.r12 = (uint64_t)(uintptr_t)&scheduler_idle_main;
    idle->arch.switch_ctx.r13 = 0U;
    idle->arch.switch_ctx.r14 = stack_top;
    idle->arch.switch_ctx.r15 = 0U;
}

/*
 * 运行队列中的 current 指针必须和当前真正使用的内核栈同步提交。
 * 本地中断若落在“已选择 next、尚未切栈”的窗口，会把旧栈误认为 next 的栈，
 * 所以调度全过程都保持关中断；线程以后恢复到这里时再恢复原来的 IF。
 */
static uint64_t scheduler_irq_save(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static void scheduler_irq_restore(uint64_t flags) {
    if ((flags & (1ULL << 9)) != 0) {
        __asm__ volatile ("sti" : : : "memory");
    } else {
        __asm__ volatile ("cli" : : : "memory");
    }
}

static void scheduler_zero(void *memory, size_t size) {
    uint8_t *bytes = (uint8_t *)memory;
    while (size-- != 0) *bytes++ = 0;
}

bool scheduler_current_cpu(uint32_t *cpu_id) {
    uint32_t current = x86_current_cpu_index();
    if (cpu_id == 0 || current >= g_cpu_count || !x86_smp_cpu_online(current)) {
        return false;
    }
    *cpu_id = current;
    return true;
}

uint64_t scheduler_lock(spinlock_t *lock) {
    uint64_t flags = scheduler_irq_save();
    while (atomic_exchange_explicit(&lock->state, 1U, memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
    return flags;
}

/*
 * Local scheduler entry points already disabled interrupts before taking
 * the local runqueue lock. Avoid a second IRQ-save/restore pair there.
 */
static void scheduler_lock_irq_disabled(spinlock_t *lock) {
    while (atomic_exchange_explicit(&lock->state, 1U,
                                    memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
}

static void scheduler_unlock_irq_disabled(spinlock_t *lock) {
    atomic_store_explicit(&lock->state, 0U, memory_order_release);
}

void scheduler_unlock(spinlock_t *lock, uint64_t flags) {
    atomic_store_explicit(&lock->state, 0U, memory_order_release);
    scheduler_irq_restore(flags);
}

/*
 * exec_start_ns is a runtime-accounting anchor, not a lifetime timestamp.
 * Keep it valid only while its thread owns the CPU.  The run-queue lock also
 * serializes this with the state and current-thread transition in schedule().
 */
static void scheduler_accounting_transition(run_queue_t *queue,
                                            thread_t *current, thread_t *next) {
    if (queue == 0) return;
    if (current != 0 && current != queue->idle) {
        current->sched.exec_start_ns = 0;
    }
    if (next != 0 && next != queue->idle) {
        next->sched.exec_start_ns = queue->clock_ns;
    }
}

bool sched_accounting_self_test(void) {
    run_queue_t queue;
    thread_t idle;
    thread_t first;
    thread_t second;

    scheduler_zero(&queue, sizeof(queue));
    scheduler_zero(&idle, sizeof(idle));
    scheduler_zero(&first, sizeof(first));
    scheduler_zero(&second, sizeof(second));
    queue.idle = &idle;
    queue.clock_ns = 100U;
    idle.sched.exec_start_ns = 33U;
    first.sched.exec_start_ns = 10U;
    second.sched.exec_start_ns = 20U;

    scheduler_accounting_transition(&queue, &first, &second);
    if (first.sched.exec_start_ns != 0U || second.sched.exec_start_ns != 100U ||
        idle.sched.exec_start_ns != 33U) {
        return false;
    }

    queue.clock_ns = 200U;
    scheduler_accounting_transition(&queue, &second, &second);
    if (second.sched.exec_start_ns != 200U) return false;

    scheduler_accounting_transition(&queue, &second, &idle);
    return second.sched.exec_start_ns == 0U && idle.sched.exec_start_ns == 33U;
}

void sched_cpu_init(uint32_t cpu_id) {
    if (cpu_id >= MAX_CPUS) return;
    scheduler_cpu_t *cpu = &g_cpus[cpu_id];
    scheduler_zero(cpu, sizeof(*cpu));
    atomic_init(&cpu->queue.lock.state, 0U);
    atomic_init(&cpu->queue_snapshot, 0U);
    atomic_init(&cpu->reap_deferred_queued, false);
    cpu->queue.fair_root.root = 0;
    for (uint32_t priority = 0; priority < RT_PRIORITY_LEVELS; ++priority) {
        list_init(&cpu->queue.rt_queues[priority]);
    }
    initialize_thread(&g_idle_threads[cpu_id], (uint64_t)cpu_id, SCHED_CLASS_IDLE, 0);
    atomic_store_explicit(&g_idle_threads[cpu_id].state, THREAD_RUNNING, memory_order_relaxed);
    g_idle_threads[cpu_id].current_cpu = (uint16_t)cpu_id;
    cpu->queue.idle = &g_idle_threads[cpu_id];
    cpu->queue.current = cpu->queue.idle;
    scheduler_publish_queue_snapshot(cpu);
    cpu->idle_stack_top = x86_cpu_kernel_stack(cpu_id);
    scheduler_init_idle_context(&g_idle_threads[cpu_id], cpu->idle_stack_top);
    if (cpu_id + 1U > g_cpu_count) g_cpu_count = cpu_id + 1U;
}

void sched_init(void) {
    g_cpu_count = 0;
    g_kernel_root = x86_current_root_table();
    uint32_t discovered = x86_smp_discovered_count();
    for (uint32_t cpu_id = 0; cpu_id < discovered && cpu_id < MAX_CPUS; ++cpu_id) {
        if (x86_smp_cpu_started(cpu_id)) sched_cpu_init(cpu_id);
    }
}

bool sched_set_boot_kernel_stack(vaddr_t stack_top) {
    if (stack_top == 0 || (stack_top & 15U) != 0 || !x86_is_canonical(stack_top)) {
        return false;
    }
    /* 仅在 BSP 引导路径、关中断状态下调用，因此不需要获取运行队列锁。 */
    uint32_t cpu_id;
    if (!scheduler_current_cpu(&cpu_id)) return false;
    g_cpus[cpu_id].idle_stack_top = stack_top;
    return true;
}

static bool sched_enqueue_internal(thread_t *thread, bool notify_local) {
    if (thread == 0 || thread->sched_class == SCHED_CLASS_IDLE || g_cpu_count == 0) {
        return false;
    }
    uint32_t current_cpu;
    if (!scheduler_current_cpu(&current_cpu)) return false;
    uint32_t cpu_id = scheduler_cpu_available(thread->current_cpu) ?
                      thread->current_cpu : current_cpu;
    /* 只有具备真实内核保存栈的线程才参与跨 CPU 分配；纯结构自检固定在指定队列。 */
    /*
     * 首次放置也按全局负载选择 CPU。
     * 远端 idle CPU 的首次运行由 reschedule IPI 唤醒，并在 idle loop
     * 中消费 pending request，因此不再需要强制 creator-local。
     */
    if ((thread->sched.flags & SCHED_ENTITY_INITIAL_PLACEMENT) != 0) {
        cpu_id = scheduler_choose_cpu(thread, current_cpu);
        thread->sched.flags &= (uint16_t)~SCHED_ENTITY_INITIAL_PLACEMENT;
    }
    scheduler_cpu_t *cpu = &g_cpus[cpu_id];
    uint64_t queue_flags = scheduler_lock(&cpu->queue.lock);
    uint32_t before = cpu->queue.nr_running;
    thread->current_cpu = (uint16_t)cpu_id;
    enqueue_locked(cpu, thread);
    bool enqueued = cpu->queue.nr_running != before;
    bool notify = enqueued &&
                  (cpu_id != current_cpu ||
                   (notify_local && cpu->queue.current != cpu->queue.idle));
    scheduler_unlock(&cpu->queue.lock, queue_flags);
    if (notify) {
        /*
         * Remote CPUs always need an IPI.  Normal local enqueue also uses a
         * self-IPI so a real current thread observes prompt preemption.  The
         * boot-only caller disables that local notification explicitly while
         * the BSP is still executing on its unsaved bootstrap continuation.
         */
        (void)x86_smp_request_reschedule(cpu_id);
    }
    return enqueued;
}

bool sched_enqueue(thread_t *thread) {
    return sched_enqueue_internal(thread, true);
}

bool sched_enqueue_bootstrap(thread_t *thread) {
    return sched_enqueue_internal(thread, false);
}

kstatus_t sched_start_thread(thread_t *thread) {
    if (thread == 0) return K_EINVAL;

    unsigned expected = THREAD_NEW;
    if (!atomic_compare_exchange_strong_explicit(
            &thread->state, &expected, THREAD_READY,
            memory_order_acq_rel, memory_order_acquire)) {
        return expected == THREAD_READY || expected == THREAD_RUNNING ?
               K_EBUSY : K_EINVAL;
    }

    /* The scheduler publishes READY and owns the corresponding queue insert. */
    (void)sched_enqueue(thread);
    return K_OK;
}

bool sched_publish_blocked(thread_t *thread) {
    if (thread == 0) return false;

    unsigned expected = THREAD_RUNNING;
    return atomic_compare_exchange_strong_explicit(
        &thread->state, &expected, THREAD_BLOCKED,
        memory_order_acq_rel, memory_order_acquire);
}

bool sched_publish_dead(thread_t *thread) {
    if (thread == 0) return false;

    uint32_t cpu_id;
    bool current = scheduler_current_cpu(&cpu_id) &&
                   cpu_id < MAX_CPUS && g_cpus[cpu_id].queue.current == thread;
    unsigned state = atomic_load_explicit(&thread->state, memory_order_acquire);
    for (;;) {
        if (state == THREAD_DEAD) return false;
        if (state == THREAD_BLOCKED ||
            (state == THREAD_RUNNING && !current)) return false;

        if (atomic_compare_exchange_weak_explicit(
                &thread->state, &state, THREAD_DEAD,
                memory_order_release, memory_order_acquire)) {
            return true;
        }
    }
}

void sched_wake(thread_t *thread) {
    if (thread == 0 || g_cpu_count == 0) return;

    /*
     * The wait queue owns the waiter state transition, while this function
     * owns the thread state transition.  They are deliberately independent:
     * a waiter may already have left its queue when the scheduler observes the
     * blocked thread.  Do not gate this wake on the stack-local waiter; doing
     * so can lose a child/fence wake during the hand-off between two CPUs.
     */
    uint32_t caller_cpu = x86_current_cpu_index();
    uint32_t cpu_id = scheduler_cpu_available(thread->current_cpu) ?
                      thread->current_cpu : caller_cpu;
    if (!scheduler_cpu_available(cpu_id)) return;
    scheduler_cpu_t *cpu = &g_cpus[cpu_id];
    uint64_t queue_flags = scheduler_lock(&cpu->queue.lock);
    unsigned expected = THREAD_BLOCKED;
    bool woke = atomic_compare_exchange_strong_explicit(
        &thread->state,
        &expected,
        THREAD_READY,
        memory_order_acq_rel,
        memory_order_relaxed);
    if (!woke) {
        scheduler_unlock(&cpu->queue.lock, queue_flags);
        return;
    }

    /*
     * 若线程仍是该 CPU 的 current，它还没有保存完内核上下文。这里只改变状态，
     * 随后的 schedule() 会在同一把锁下重新入队；否则可直接发布到原 CPU。
     * 阻塞唤醒暂不迁移，避免同一内核栈在两个 CPU 上同时执行。
    */
    if (cpu->queue.current != thread) {
        enqueue_locked(cpu, thread);
    }
    scheduler_unlock_irq_disabled(&cpu->queue.lock);
    /*
     * A blocked current thread is briefly still published as queue.current
     * until the blocking path switches away.  In that window it is not
     * re-enqueued, but the successful BLOCKED -> READY transition still
     * needs to interrupt the target CPU so the pending switch cannot be
     * stranded behind an unrelated timer tick.
     */
    if (woke) {
        (void)x86_smp_request_reschedule(cpu_id);
    }
}

static void scheduler_reap_enqueue_locked(scheduler_cpu_t *cpu,
                                           thread_t *thread) {
    if (cpu == 0 || thread == 0) return;

    uint32_t flags =
        __atomic_load_n(&thread->flags, __ATOMIC_ACQUIRE);
    uint16_t sched_flags = thread->sched.flags;

    /*
     * 没有 execution ref 的线程不需要 deferred reap。
     * QUEUED 防止异常的重复 schedule 把同一线程加入两次。
     */
    if ((flags & THREAD_FLAG_EXECUTION_REF) == 0U ||
        (sched_flags & SCHED_ENTITY_REAP_QUEUED) != 0U) {
        return;
    }

    thread->sched.flags |= SCHED_ENTITY_REAP_QUEUED;

    thread->reap_next = 0;

    if (cpu->reap_tail != 0) {
        cpu->reap_tail->reap_next = thread;
    } else {
        cpu->reap_head = thread;
    }

    cpu->reap_tail = thread;
}

void schedule(void) {
    uint64_t irq_flags = scheduler_irq_save();
    if (sched_preempt_disabled()) {
        scheduler_irq_restore(irq_flags);
        return;
    }
    uint32_t cpu_id;
    if (!scheduler_current_cpu(&cpu_id)) {
        scheduler_irq_restore(irq_flags);
        return;
    }
    scheduler_cpu_t *cpu = &g_cpus[cpu_id];
    scheduler_lock_irq_disabled(&cpu->queue.lock);
    thread_t *current = cpu->queue.current;
    if (current != 0 && current != cpu->queue.idle) {
        unsigned current_state = atomic_load_explicit(&current->state,
                                                       memory_order_relaxed);
        if (current_state == THREAD_RUNNING || current_state == THREAD_READY) {
            if (current_state == THREAD_RUNNING) {
                atomic_store_explicit(&current->state, THREAD_READY,
                                      memory_order_relaxed);
            }
            enqueue_locked(cpu, current);
        }
    }
    thread_t *next = pick_locked(cpu);
    if (next != 0) {
        dequeue_locked(cpu, next);
        atomic_store_explicit(&next->state, THREAD_RUNNING, memory_order_relaxed);
        next->current_cpu = (uint16_t)cpu_id;
        cpu->queue.current = next;
    }
    scheduler_publish_queue_snapshot(cpu);
    scheduler_accounting_transition(&cpu->queue, current, next);
    /*
     * DEAD current 的 execution ref 只能在真正离开它的栈/CR3 后释放。
     *
     * 使用 FIFO，不能因为前一个待回收线程尚未处理就丢掉当前线程。
     */
    if (current != 0 &&
        current != cpu->queue.idle &&
        atomic_load_explicit(&current->state,
                             memory_order_relaxed) == THREAD_DEAD &&
        next != 0 &&
        next != current &&
        current->arch.switch_ctx.rsp != 0 &&
        next->arch.switch_ctx.rsp != 0) {

        scheduler_reap_enqueue_locked(cpu, current);
    }
    scheduler_unlock(&cpu->queue.lock, queue_flags);

    /*
     * 只有具备真实保存栈的线程才触发架构切换。这样早期纯数据结构自检仍可使用
     * 临时 thread_t，而正式线程会同步 CR3、TSS.RSP0、FS.base 和系统调用栈。
     */
    if (next != 0 && next != current && current != 0 &&
        current->arch.switch_ctx.rsp != 0 &&
        next->arch.switch_ctx.rsp != 0) {
        x86_cpu_local_t *local = x86_cpu_local_current();
        if (local != 0) {
            /*
             * A syscall may explicitly block and call schedule().  Keep
             * that fact with the thread so the next thread on this CPU does
             * not inherit the caller's syscall-only interrupt policy.
             */
            current->arch.syscall_active = local->SyscallActive;
            local->SyscallActive = next == cpu->queue.idle ? 0U :
                                   next->arch.syscall_active;
        }
        paddr_t root = g_kernel_root;
        vaddr_t kernel_stack = cpu->idle_stack_top;
        if (next->process != 0 && next->process->vm != 0) {
            root = next->process->vm->root_table;
            kernel_stack = next->kernel_stack_top;
            /* vmalloc 等晚期内核映射在切换前同步到目标 PML4。 */
            x86_sync_kernel_half(root, g_kernel_root);
        }
        x86_tss_set_rsp0(kernel_stack);
        x86_syscall_set_kernel_stack(kernel_stack);
        x86_set_user_fs_base(next->arch.fs_base);
        x86_fp_switch(&current->arch, &next->arch);
        paddr_t cr3 = paddr_make(x86_cr3_value(
            root, next->process != 0 && next->process->vm != 0 ?
                next->process->vm->pcid : 0U));
        x86_switch_context_root(&current->arch.switch_ctx, &next->arch.switch_ctx, cr3);
    }
    scheduler_irq_restore(irq_flags);
    /* 回收可能发起跨 CPU TLB shootdown，必须在 IF=1 时执行。 */
    sched_finish_switch();
}

bool sched_try_run_ready(void) {
    uint64_t irq_flags = scheduler_irq_save();
    uint32_t cpu_id;
    if (!scheduler_current_cpu(&cpu_id)) {
        scheduler_irq_restore(irq_flags);
        return false;
    }
    scheduler_cpu_t *cpu = &g_cpus[cpu_id];
    scheduler_lock_irq_disabled(&cpu->queue.lock);
    thread_t *next = pick_locked(cpu);
    /* 丢弃没有真实上下文的早期自检线程，避免空闲 CPU 被假线程锁死。 */
    while (next != 0 && next != cpu->queue.idle &&
           next->arch.switch_ctx.rsp == 0) {
        dequeue_locked(cpu, next);
        atomic_store_explicit(&next->state, THREAD_READY, memory_order_release);
        next = pick_locked(cpu);
    }
    bool runnable = next != 0 && next != cpu->queue.idle;
    scheduler_unlock_irq_disabled(&cpu->queue.lock);
    scheduler_irq_restore(irq_flags);
    if (runnable) schedule();
    return runnable;
}

void sched_preempt_disable(void) {
    /* GS is CPU-private: one local RMW is enough, no locked atomic needed. */
    x86_preempt_disable_fast();
}

void sched_preempt_enable(void) {
    if (!x86_preempt_disabled_fast()) return;
    x86_preempt_enable_fast();
}

bool sched_preempt_disabled(void) {
    return x86_preempt_disabled_fast();
}

/*
 * Pop one thread whose CPU has already switched away from its old stack/CR3.
 *
 * This helper intentionally accepts an explicit CPU id: once the originating
 * CPU has reached its post-switch continuation, the actual object/VM teardown
 * may safely execute on the global deferred worker.
 */
static thread_t *scheduler_reap_pop_cpu(uint32_t cpu_id) {
    if (cpu_id >= g_cpu_count) return 0;

    scheduler_cpu_t *cpu = &g_cpus[cpu_id];

    uint64_t queue_flags =
        scheduler_lock(&cpu->queue.lock);

    thread_t *thread = cpu->reap_head;

    if (thread != 0) {
        cpu->reap_head = thread->reap_next;

        if (cpu->reap_head == 0) {
            cpu->reap_tail = 0;
        }

        thread->reap_next = 0;

        /*
         * From this point the FIFO no longer owns the entry.
         * EXECUTION_REF itself continues to protect thread lifetime until
         * thread_release_execution_ref() finishes.
         */
        thread->sched.flags &= (uint16_t)~SCHED_ENTITY_REAP_QUEUED;
    }

    scheduler_unlock(&cpu->queue.lock,
                     queue_flags);

    return thread;
}

static bool scheduler_reap_cpu_pending(uint32_t cpu_id) {
    if (cpu_id >= g_cpu_count) return false;

    scheduler_cpu_t *cpu = &g_cpus[cpu_id];

    uint64_t queue_flags =
        scheduler_lock(&cpu->queue.lock);

    bool pending = cpu->reap_head != 0;

    scheduler_unlock(&cpu->queue.lock,
                     queue_flags);

    return pending;
}

static void scheduler_reap_drain_cpu(uint32_t cpu_id) {
    for (;;) {
        thread_t *thread =
            scheduler_reap_pop_cpu(cpu_id);

        if (thread == 0) {
            break;
        }

        /*
         * No scheduler/runqueue lock is held here.
         *
         * This may destroy process VM objects and issue TLB shootdowns.
         */
        thread_release_execution_ref(thread);
    }
}

static void scheduler_reap_deferred_work(void *argument);

static void scheduler_reap_schedule_deferred(uint32_t cpu_id) {
    if (cpu_id >= g_cpu_count) return;

    scheduler_cpu_t *cpu = &g_cpus[cpu_id];

    if (!scheduler_reap_cpu_pending(cpu_id)) {
        return;
    }

    bool expected = false;

    if (!atomic_compare_exchange_strong_explicit(
            &cpu->reap_deferred_queued,
            &expected,
            true,
            memory_order_acq_rel,
            memory_order_acquire)) {
        return;
    }

    /*
     * This function can be reached from LAPIC hard IRQ context, therefore
     * only the IRQ-safe deferred producer is legal here.
     */
    if (!deferred_try_schedule(
            scheduler_reap_deferred_work,
            (void *)(uintptr_t)cpu_id)) {

        /*
         * Queue full: keep the reaper entries intact and permit the next
         * timer/schedule boundary to retry.
         */
        atomic_store_explicit(
            &cpu->reap_deferred_queued,
            false,
            memory_order_release);
    }
}

static void scheduler_reap_deferred_work(void *argument) {
    uint32_t cpu_id =
        (uint32_t)(uintptr_t)argument;

    if (cpu_id >= g_cpu_count) return;

    scheduler_cpu_t *cpu =
        &g_cpus[cpu_id];

    /*
     * We are now in ordinary Ring0 worker context with interrupts enabled.
     * The originating CPU had already completed the actual context switch
     * before this item was published.
     */
    scheduler_reap_drain_cpu(cpu_id);

    /*
     * Drop the coalescing ownership only after the current drain completes.
     */
    atomic_store_explicit(
        &cpu->reap_deferred_queued,
        false,
        memory_order_release);

    /*
     * Close:
     *
     * drain sees empty
     *       ↓
     * CPU queues another DEAD thread while queued==true
     *       ↓
     * worker clears queued
     *
     * race.
     */
    if (scheduler_reap_cpu_pending(cpu_id)) {
        scheduler_reap_schedule_deferred(cpu_id);
    }
}

void sched_finish_switch(void) {
    uint64_t flags;

    __asm__ volatile (
        "pushfq; popq %0"
        : "=r"(flags)
        :
        : "memory");

    uint32_t cpu_id;

    if (!scheduler_current_cpu(&cpu_id)) {
        return;
    }

    if ((flags & (1ULL << 9)) == 0) {
        /*
         * Typical case:
         *
         * timer IRQ
         *   -> sched_tick()
         *   -> schedule()
         *   -> context switch
         *
         * The old stack/CR3 is already inactive, but we are still inside the
         * restored interrupt call chain.  VM/TLB teardown must therefore be
         * deferred rather than simply discarded.
         */
        scheduler_reap_schedule_deferred(cpu_id);
        return;
    }

    /*
     * Normal process/kernel scheduling boundary: teardown can execute
     * synchronously.
     */
    scheduler_reap_drain_cpu(cpu_id);
}

void sched_block_current(void) {
    uint32_t cpu_id;
    if (!scheduler_current_cpu(&cpu_id)) return;
    thread_t *current = g_cpus[cpu_id].queue.current;
    if (current == 0 || current == g_cpus[cpu_id].queue.idle) return;
    if (sched_publish_blocked(current)) schedule();
}

bool sched_state_transition_self_test(void) {
    thread_t thread = {0};
    atomic_init(&thread.state, THREAD_RUNNING);
    atomic_init(&thread.blocked_waiter, 0);

    if (!sched_publish_blocked(&thread) ||
        atomic_load_explicit(&thread.state, memory_order_acquire) !=
            THREAD_BLOCKED ||
        sched_publish_dead(&thread)) {
        return false;
    }

    atomic_store_explicit(&thread.state, THREAD_READY, memory_order_release);
    if (!sched_publish_dead(&thread) ||
        atomic_load_explicit(&thread.state, memory_order_acquire) != THREAD_DEAD ||
        sched_publish_dead(&thread)) {
        return false;
    }
    return true;
}

static void sched_context_test_entry(void) {
    g_context_test_ran = 1U;
    x86_switch_context(g_context_test_to, g_context_test_from);
    for (;;) __asm__ volatile ("cli; hlt" : : : "memory");
}

bool sched_context_switch_self_test(void) {
    static uint8_t stack[4096] __attribute__((aligned(16)));
    arch_switch_context_t from = {0};
    arch_switch_context_t to = {0};
    uintptr_t stack_top = ((uintptr_t)stack + sizeof(stack)) & ~(uintptr_t)15U;

    stack_top -= sizeof(uint64_t);
    *(uint64_t *)stack_top = (uint64_t)(uintptr_t)&sched_context_test_entry;
    to.rsp = stack_top;
    g_context_test_from = &from;
    g_context_test_to = &to;
    g_context_test_ran = 0U;
    uint64_t benchmark_start = telemetry_timestamp();
    x86_switch_context(&from, &to);
    kernel_perf_emit_scope("scheduler.context_switch", benchmark_start);
    g_context_test_from = 0;
    g_context_test_to = 0;
    return g_context_test_ran == 1U;
}

kstatus_t sched_set_affinity(thread_t *thread, const cpumask_t *mask) {
    if (thread == 0 || mask == 0 || g_cpu_count == 0) return K_EINVAL;
    bool any = false;
    for (uint32_t word = 0; word < MAX_CPUS / 64U; ++word) {
        thread->affinity.bits[word] = mask->bits[word];
        if (mask->bits[word] != 0) any = true;
    }
    return any ? K_OK : K_EINVAL;
}

void sched_set_effective_priority(thread_t *thread, uint8_t class_id,
                                  uint8_t rt_priority) {
    if (thread == 0 || g_cpu_count == 0 ||
        (class_id != SCHED_CLASS_RT && class_id != SCHED_CLASS_FAIR) ||
        (class_id == SCHED_CLASS_RT && rt_priority >= RT_PRIORITY_LEVELS)) return;
    uint32_t current_cpu;
    if (!scheduler_current_cpu(&current_cpu)) return;
    uint32_t cpu_id = scheduler_cpu_available(thread->current_cpu) ?
                      thread->current_cpu : current_cpu;
    scheduler_cpu_t *cpu = &g_cpus[cpu_id];
    uint64_t queue_flags = scheduler_lock(&cpu->queue.lock);
    bool enqueued = (thread->sched.flags & SCHED_ENTITY_ENQUEUED) != 0;
    if (enqueued) dequeue_locked(cpu, thread);
    thread->sched_class = class_id;
    thread->rt_priority = class_id == SCHED_CLASS_RT ? rt_priority : 0U;
    if (enqueued) enqueue_locked(cpu, thread);
    scheduler_unlock(&cpu->queue.lock, queue_flags);
}

thread_t *sched_current_thread(void) {
    uint32_t cpu_id;
    if (!scheduler_current_cpu(&cpu_id)) return 0;
    return g_cpus[cpu_id].queue.current;
}

void sched_remove(thread_t *thread) {
    if (thread == 0 || g_cpu_count == 0) return;
    uint32_t current_cpu;
    if (!scheduler_current_cpu(&current_cpu)) return;
    uint32_t cpu_id = scheduler_cpu_available(thread->current_cpu) ?
                      thread->current_cpu : current_cpu;
    scheduler_cpu_t *cpu = &g_cpus[cpu_id];
    uint64_t queue_flags = scheduler_lock(&cpu->queue.lock);
    if (cpu->queue.current != thread) dequeue_locked(cpu, thread);
    scheduler_unlock(&cpu->queue.lock, queue_flags);
}

uint32_t sched_runnable_count(void) {
    uint32_t cpu_id;
    if (!scheduler_current_cpu(&cpu_id)) return 0;
    scheduler_cpu_t *cpu = &g_cpus[cpu_id];
    return scheduler_snapshot_runnable(cpu);
}

bool sched_validate_current_cpu(void) {
    uint32_t cpu_id;
    if (!scheduler_current_cpu(&cpu_id)) return false;
    scheduler_cpu_t *cpu = &g_cpus[cpu_id];
    uint64_t queue_flags = scheduler_lock(&cpu->queue.lock);
    bool valid = cpu->queue.fair_root.root == 0 ||
                 rb_tree_color(cpu->queue.fair_root.root) == RB_TREE_BLACK;
    thread_t *previous = 0;
    uint32_t black_depth = UINT32_MAX;
    uint32_t fair_count = 0;
    if (valid) {
        valid = validate_fair_node(cpu->queue.fair_root.root, 0, &previous, 0,
                                   &black_depth, &fair_count) &&
                fair_count == cpu->queue.fair_count;
    }
    uint32_t rt_count = 0;
    for (uint32_t priority = 0; valid && priority < RT_PRIORITY_LEVELS; ++priority) {
        bool empty = list_empty(&cpu->queue.rt_queues[priority]);
        if (empty != ((cpu->queue.rt_bitmap & (1U << priority)) == 0)) valid = false;
        for (list_head_t *item = cpu->queue.rt_queues[priority].next;
             item != &cpu->queue.rt_queues[priority]; item = item->next) {
            if (++rt_count > cpu->queue.nr_running) {
                valid = false;
                break;
            }
        }
    }
    valid = valid && fair_count + rt_count == cpu->queue.nr_running;
    scheduler_unlock(&cpu->queue.lock, queue_flags);
    return valid;
}

bool sched_debug_cpu(uint32_t cpu_id, uint32_t *current_state,
                     uint64_t *current_tid, uint32_t *runnable_count) {
    if (cpu_id >= g_cpu_count || g_cpus[cpu_id].queue.idle == 0) return false;
    scheduler_cpu_t *cpu = &g_cpus[cpu_id];
    uint64_t queue_flags = scheduler_lock(&cpu->queue.lock);
    thread_t *current = cpu->queue.current;
    if (current_state != 0) {
        *current_state = current != 0 ?
            atomic_load_explicit(&current->state, memory_order_acquire) : UINT32_MAX;
    }
    if (current_tid != 0) *current_tid = current != 0 ? current->tid : 0U;
    if (runnable_count != 0) *runnable_count = cpu->queue.nr_running;
    scheduler_unlock(&cpu->queue.lock, queue_flags);
    return true;
}

/*
 * 用户地址 TLB shootdown 的目标筛选。
 *
 * queue.current 在 schedule() 中先于 CR3 切换发布。切入目标地址空间时最多
 * 多发一次 IPI；切出时 CPU 已只执行内核切换代码，不再访问旧用户映射。
 * x86_switch_context_root() 的 MOV CR3 当前没有设置 PCID no-flush(bit 63)，
 * 因此已经切出的 CPU 下次重新进入该 PCID 时会刷新旧 translation。
 */
bool sched_cpu_uses_root(uint32_t cpu_id, paddr_t root) {
    if (root.value == 0 || cpu_id >= g_cpu_count ||
        g_cpus[cpu_id].queue.idle == 0) return false;

    scheduler_cpu_t *cpu = &g_cpus[cpu_id];
    uint64_t queue_flags = scheduler_lock(&cpu->queue.lock);
    thread_t *current = cpu->queue.current;
    paddr_t current_root = g_kernel_root;
    if (current != 0 && current->process != 0 && current->process->vm != 0) {
        current_root = current->process->vm->root_table;
    }
    bool uses_root = current_root.value == root.value;
    scheduler_unlock(&cpu->queue.lock, queue_flags);
    return uses_root;
}
