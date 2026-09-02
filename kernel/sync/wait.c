#include <arch/x86_64/cpu.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/wait.h>

static spinlock_t g_timeout_lock;
static list_head_t g_timeout_waiters;
static atomic_uint g_wait_init_state;

static uint64_t wait_lock(spinlock_t *lock) {
    /*
     * 持有等待系统的锁时不能被本地时钟抢占。定时器维护路径也会
     * 访问 g_timeout_lock；如果锁的持有者被切走，当前线程会永久
     * 自旋在这里，直到持有者重新获得 CPU。
     */
    uint64_t flags;
    sched_preempt_disable();
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    while (atomic_exchange_explicit(&lock->state, 1U, memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
    return flags;
}

static void wait_unlock(spinlock_t *lock, uint64_t flags) {
    atomic_store_explicit(&lock->state, 0U, memory_order_release);
    sched_preempt_enable();
    if ((flags & (1ULL << 9)) != 0U) {
        __asm__ volatile ("sti" : : : "memory");
    } else {
        __asm__ volatile ("cli" : : : "memory");
    }
}

static void wait_insert_tail(list_head_t *head, list_head_t *node) {
    node->next = head;
    node->prev = head->prev;
    head->prev->next = node;
    head->prev = node;
}

static void wait_remove(list_head_t *node) {
    node->prev->next = node->next;
    node->next->prev = node->prev;
    list_init(node);
}

static void waiter_unlink_locked(waiter_t *waiter) {
    if (waiter == 0) return;
    if (waiter->node.next != &waiter->node) {
        wait_remove(&waiter->node);
    }
    if (waiter->timeout_node.next != &waiter->timeout_node) {
        wait_remove(&waiter->timeout_node);
    }
}

static waiter_t *waiter_from_node(list_head_t *node) {
    return (waiter_t *)((uint8_t *)node - __builtin_offsetof(waiter_t, node));
}

static waiter_t *waiter_from_timeout_node(list_head_t *node) {
    return (waiter_t *)((uint8_t *)node -
                        __builtin_offsetof(waiter_t, timeout_node));
}

static void wait_global_init(void) {
    unsigned expected = 0;
    if (atomic_compare_exchange_strong_explicit(&g_wait_init_state, &expected, 1U,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        atomic_init(&g_timeout_lock.state, 0U);
        list_init(&g_timeout_waiters);
        atomic_store_explicit(&g_wait_init_state, 2U, memory_order_release);
        return;
    }
    while (atomic_load_explicit(&g_wait_init_state, memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
}

static bool deadline_reached(uint64_t now, uint64_t deadline) {
    return deadline != UINT64_MAX && (int64_t)(now - deadline) >= 0;
}

static uint64_t make_deadline(uint64_t timeout_ns) {
    if (timeout_ns == UINT64_MAX) return UINT64_MAX;
    uint64_t now = x86_read_tsc();
    uint64_t ticks = x86_timeout_ns_to_tsc(timeout_ns);
    return ticks > UINT64_MAX - now ? UINT64_MAX : now + ticks;
}

void wait_queue_init(wait_queue_t *queue) {
    if (queue == 0) return;
    wait_global_init();
    atomic_init(&queue->lock.state, 0U);
    list_init(&queue->waiters);
    atomic_init(&queue->sequence, 0U);
}

static kstatus_t wait_without_scheduler(bool (*predicate)(void *), void *context,
                                        uint64_t deadline) {
    for (;;) {
        if (predicate(context)) return K_OK;
        if (deadline_reached(x86_read_tsc(), deadline)) return K_ETIMEDOUT;
        __asm__ volatile ("pause");
    }
}

kstatus_t wait_on_queue(wait_queue_t *queue, bool (*predicate)(void *), void *context,
                        uint64_t timeout_ns) {
    if (queue == 0 || predicate == 0) return K_EINVAL;
    wait_global_init();
    uint64_t deadline = make_deadline(timeout_ns);
    thread_t *thread = sched_current_thread();
    bool can_schedule = thread != 0 && thread->object.type == KOBJECT_TYPE_THREAD &&
                        thread->sched_class != SCHED_CLASS_IDLE;
    if (!can_schedule) return wait_without_scheduler(predicate, context, deadline);

    waiter_t waiter;
    list_init(&waiter.node);
    list_init(&waiter.timeout_node);
    waiter.thread = thread;
    waiter.queue = queue;
    waiter.flags = 0;
    waiter.deadline_tsc = deadline;

    for (;;) {
        /* 所有涉及超时链和等待队列的路径都遵守同一锁顺序。 */
        uint64_t timeout_lock_flags = wait_lock(&g_timeout_lock);
        uint64_t queue_lock_flags = wait_lock(&queue->lock);
        unsigned queue_sequence = atomic_load_explicit(&queue->sequence,
                                                       memory_order_acquire);
        if (predicate(context)) {
            wait_unlock(&queue->lock, queue_lock_flags);
            wait_unlock(&g_timeout_lock, timeout_lock_flags);
            return K_OK;
        }
        if (deadline_reached(x86_read_tsc(), deadline)) {
            wait_unlock(&queue->lock, queue_lock_flags);
            wait_unlock(&g_timeout_lock, timeout_lock_flags);
            return K_ETIMEDOUT;
        }
        atomic_store_explicit(&waiter.state, WAITER_WAITING,
                              memory_order_relaxed);

        /*
         * Publish the owning waiter before THREAD_BLOCKED becomes visible.
         * Any CPU that successfully observes BLOCKED through sched_wake()'s
         * acquire CAS must therefore also observe blocked_waiter.
         */
        atomic_store_explicit(&thread->blocked_waiter, &waiter,
                              memory_order_release);

        if (!sched_publish_blocked(thread)) {
            atomic_store_explicit(&thread->blocked_waiter, 0,
                                  memory_order_release);
            wait_unlock(&queue->lock, queue_lock_flags);
            wait_unlock(&g_timeout_lock, timeout_lock_flags);
            return K_EBUSY;
        }

        wait_insert_tail(&queue->waiters, &waiter.node);
        if (deadline != UINT64_MAX) {
            wait_insert_tail(&g_timeout_waiters, &waiter.timeout_node);
        }

        /*
         * A producer may change its predicate state without queue->lock and
         * then block in wake_one()/wake_all() behind this lock.  A changed
         * sequence means that notification happened before insertion; remove
         * this waiter and re-evaluate the predicate before sleeping.  Do not
         * call predicate() a second time here: some predicates intentionally
         * remember their initial user-memory check (futex).
         */
        if (atomic_load_explicit(&queue->sequence,
                                 memory_order_acquire) != queue_sequence) {
            waiter_unlink_locked(&waiter);
            atomic_store_explicit(&thread->blocked_waiter, 0,
                                  memory_order_release);
            (void)sched_publish_running(thread);
            wait_unlock(&queue->lock, queue_lock_flags);
            wait_unlock(&g_timeout_lock, timeout_lock_flags);
            continue;
        }
        if (deadline_reached(x86_read_tsc(), deadline)) {
            waiter_unlink_locked(&waiter);
            atomic_store_explicit(&thread->blocked_waiter, 0,
                                  memory_order_release);
            (void)sched_publish_running(thread);
            wait_unlock(&queue->lock, queue_lock_flags);
            wait_unlock(&g_timeout_lock, timeout_lock_flags);
            return K_ETIMEDOUT;
        }
        wait_unlock(&queue->lock, queue_lock_flags);
        wait_unlock(&g_timeout_lock, timeout_lock_flags);

        /* 状态已在队列锁内置为 BLOCKED，避免发布 waiter 与真正休眠之间丢唤醒。 */
        schedule();
        unsigned state = atomic_load_explicit(&waiter.state, memory_order_acquire);
        if (state == WAITER_WAITING) {
            /*
             * A scheduler failure must not let this stack-local waiter escape
             * while it is still linked in a global queue.  Normally schedule()
             * returns only after a wake/timeout changed the state; retain this
             * defensive cancellation for early/idle transition failures.
             */
            uint64_t timeout_lock_flags = wait_lock(&g_timeout_lock);
            uint64_t queue_lock_flags = wait_lock(&queue->lock);
            unsigned expected = WAITER_WAITING;
            if (atomic_compare_exchange_strong_explicit(
                    &waiter.state, &expected, WAITER_CANCELLED,
                    memory_order_acq_rel, memory_order_acquire)) {
                waiter_unlink_locked(&waiter);
            }
            wait_unlock(&queue->lock, queue_lock_flags);
            wait_unlock(&g_timeout_lock, timeout_lock_flags);
            state = atomic_load_explicit(&waiter.state, memory_order_acquire);
        }
        atomic_store_explicit(&thread->blocked_waiter, 0,
                              memory_order_release);
        if (state == WAITER_TIMED_OUT) return K_ETIMEDOUT;
        if (state == WAITER_CANCELLED) return K_ECANCELED;
        if (state != WAITER_WOKEN) return K_EIO;
        /* 正常唤醒允许是提示性的；循环重新检查条件并在需要时重新排队。 */
    }
}

static thread_t *wake_waiter_locked(waiter_t *waiter, unsigned terminal_state) {
    unsigned expected = WAITER_WAITING;
    if (!atomic_compare_exchange_strong_explicit(&waiter->state, &expected,
                                                  terminal_state,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) return 0;
    waiter_unlink_locked(waiter);
    return waiter->thread;
}

uint32_t wake_one(wait_queue_t *queue) {
    if (queue == 0) return 0;
    uint64_t timeout_lock_flags = wait_lock(&g_timeout_lock);
    uint64_t queue_lock_flags = wait_lock(&queue->lock);
    /*
     * Keep a notification history even when the queue is momentarily empty.
     * A producer can publish the predicate state after a waiter checked it
     * and before that waiter is linked.  The sequence change lets the waiter
     * detect that notification and re-check instead of sleeping forever.
     */
    atomic_fetch_add_explicit(&queue->sequence, 1U, memory_order_release);
    for (list_head_t *node = queue->waiters.next; node != &queue->waiters;
         node = node->next) {
        waiter_t *waiter = waiter_from_node(node);
        thread_t *thread = wake_waiter_locked(waiter, WAITER_WOKEN);
        if (thread == 0) continue;
        wait_unlock(&queue->lock, queue_lock_flags);
        wait_unlock(&g_timeout_lock, timeout_lock_flags);
        sched_wake(thread);
        return 1U;
    }
    wait_unlock(&queue->lock, queue_lock_flags);
    wait_unlock(&g_timeout_lock, timeout_lock_flags);
    return 0;
}

uint32_t wake_all(wait_queue_t *queue) {
    if (queue == 0) return 0;
    uint32_t count = 0;
    while (wake_one(queue) != 0) ++count;
    return count;
}

bool wait_cancel(waiter_t *waiter) {
    if (waiter == 0 || waiter->queue == 0) return false;
    wait_queue_t *queue = waiter->queue;
    uint64_t timeout_lock_flags = wait_lock(&g_timeout_lock);
    uint64_t queue_lock_flags = wait_lock(&queue->lock);
    thread_t *thread = wake_waiter_locked(waiter, WAITER_CANCELLED);
    wait_unlock(&queue->lock, queue_lock_flags);
    wait_unlock(&g_timeout_lock, timeout_lock_flags);
    if (thread != 0) sched_wake(thread);
    return thread != 0;
}

void wait_poll_timeouts(uint64_t now_tsc) {
    if (atomic_load_explicit(&g_wait_init_state, memory_order_acquire) != 2U) return;
    for (;;) {
        waiter_t *expired = 0;
        uint64_t timeout_lock_flags = wait_lock(&g_timeout_lock);
        list_head_t *node = g_timeout_waiters.next;
        while (node != &g_timeout_waiters) {
            waiter_t *waiter = waiter_from_timeout_node(node);
            node = node->next;
            unsigned state = atomic_load_explicit(&waiter->state, memory_order_acquire);
            if (state != WAITER_WAITING) {
                wait_remove(&waiter->timeout_node);
                continue;
            }
            if (!deadline_reached(now_tsc, waiter->deadline_tsc)) continue;
            unsigned expected = WAITER_WAITING;
            if (atomic_compare_exchange_strong_explicit(&waiter->state, &expected,
                                                         WAITER_TIMED_OUT,
                                                         memory_order_acq_rel,
                                                         memory_order_acquire)) {
                wait_remove(&waiter->timeout_node);
                expired = waiter;
                break;
            }
        }
        wait_unlock(&g_timeout_lock, timeout_lock_flags);
        if (expired == 0) return;

        wait_queue_t *queue = expired->queue;
        uint64_t queue_lock_flags = wait_lock(&queue->lock);
        if (expired->node.next != &expired->node) wait_remove(&expired->node);
        wait_unlock(&queue->lock, queue_lock_flags);
        if (expired->thread != 0) sched_wake(expired->thread);
    }
}

static bool completion_try_consume(void *context) {
    completion_t *completion = (completion_t *)context;
    unsigned count = atomic_load_explicit(&completion->count, memory_order_acquire);
    while (count != 0) {
        if (atomic_compare_exchange_weak_explicit(&completion->count, &count, count - 1U,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) return true;
    }
    return false;
}

void completion_init(completion_t *completion, uint32_t initial_count) {
    if (completion == 0) return;
    atomic_init(&completion->count, initial_count);
    wait_queue_init(&completion->waitq);
}

void complete(completion_t *completion) {
    if (completion == 0) return;
    atomic_fetch_add_explicit(&completion->count, 1U, memory_order_release);
    (void)wake_one(&completion->waitq);
}

kstatus_t completion_wait(completion_t *completion, uint64_t timeout_ns) {
    if (completion == 0) return K_EINVAL;
    return wait_on_queue(&completion->waitq, completion_try_consume,
                         completion, timeout_ns);
}
