#include <kernel/deferred.h>
#include <kernel/rcu.h>
#include <kernel/spinlock.h>

#define DEFERRED_QUEUE_CAPACITY 128U

enum {
    DEFERRED_CRITICAL_EMPTY = 0U,
    DEFERRED_CRITICAL_WRITING = 1U,
    DEFERRED_CRITICAL_READY = 2U,
    DEFERRED_CRITICAL_RUNNING = 3U,
};

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
    deferred_item_t critical_item;
    atomic_uint critical_state;
    atomic_uint init_state;
} g_deferred;

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
    while (atomic_exchange_explicit(&g_deferred.lock.state, 1U,
                                    memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
    return flags;
}

static void deferred_unlock(uint64_t flags) {
    atomic_store_explicit(&g_deferred.lock.state, 0U, memory_order_release);
    deferred_irq_restore(flags);
}

bool deferred_init(void) {
    unsigned expected = 0U;
    if (atomic_compare_exchange_strong_explicit(&g_deferred.init_state, &expected, 1U,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        atomic_init(&g_deferred.lock.state, 0U);
        g_deferred.head = 0;
        g_deferred.tail = 0;
        g_deferred.count = 0;
        g_deferred.critical_item.function = 0;
        g_deferred.critical_item.argument = 0;
        atomic_init(&g_deferred.critical_state, DEFERRED_CRITICAL_EMPTY);
        atomic_store_explicit(&g_deferred.init_state, 2U, memory_order_release);
        return true;
    }
    while (atomic_load_explicit(&g_deferred.init_state, memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
    return true;
}

bool deferred_schedule(deferred_work_fn_t function, void *argument) {
    uint64_t flags;
    if (function == 0 || !deferred_init()) return false;
    flags = deferred_lock();
    if (g_deferred.count == DEFERRED_QUEUE_CAPACITY) {
        deferred_unlock(flags);
        return false;
    }
    g_deferred.items[g_deferred.tail].function = function;
    g_deferred.items[g_deferred.tail].argument = argument;
    g_deferred.tail = (g_deferred.tail + 1U) % DEFERRED_QUEUE_CAPACITY;
    ++g_deferred.count;
    deferred_unlock(flags);
    return true;
}

bool deferred_try_schedule(deferred_work_fn_t function, void *argument) {
    uint64_t flags;
    if (function == 0 || !deferred_init()) return false;
    flags = deferred_lock();
    if (g_deferred.count == DEFERRED_QUEUE_CAPACITY) {
        deferred_unlock(flags);
        return false;
    }
    g_deferred.items[g_deferred.tail].function = function;
    g_deferred.items[g_deferred.tail].argument = argument;
    g_deferred.tail = (g_deferred.tail + 1U) % DEFERRED_QUEUE_CAPACITY;
    ++g_deferred.count;
    deferred_unlock(flags);
    return true;
}

bool deferred_schedule_critical(deferred_work_fn_t function, void *argument) {
    unsigned expected = DEFERRED_CRITICAL_EMPTY;
    /*
     * This single slot is deliberately reserved for the xHCI MSI-X worker.
     * Its caller owns a coalescing queued bit, so a READY/RUNNING item is
     * already sufficient to cover every later interrupt.  Do not turn this
     * into a shared fallback queue without adding per-source ownership.
     */
    if (function == 0 || !deferred_init()) return false;
    if (!atomic_compare_exchange_strong_explicit(&g_deferred.critical_state,
                                                 &expected,
                                                 DEFERRED_CRITICAL_WRITING,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        return false;
    }
    g_deferred.critical_item.function = function;
    g_deferred.critical_item.argument = argument;
    atomic_store_explicit(&g_deferred.critical_state, DEFERRED_CRITICAL_READY,
                          memory_order_release);
    return true;
}

static bool deferred_take_critical(deferred_item_t *item) {
    unsigned expected = DEFERRED_CRITICAL_READY;
    if (item == 0 ||
        !atomic_compare_exchange_strong_explicit(&g_deferred.critical_state,
                                                 &expected,
                                                 DEFERRED_CRITICAL_RUNNING,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        return false;
    }
    *item = g_deferred.critical_item;
    atomic_store_explicit(&g_deferred.critical_state, DEFERRED_CRITICAL_EMPTY,
                          memory_order_release);
    return item->function != 0;
}

uint32_t deferred_run(uint32_t budget) {
    uint32_t completed = 0;
    if (budget == 0U || !deferred_init()) return 0;
    while (completed < budget) {
        deferred_item_t item = {0};
        bool have_item = deferred_take_critical(&item);
        if (!have_item) {
            uint64_t flags = deferred_lock();
            if (g_deferred.count != 0U) {
                item = g_deferred.items[g_deferred.head];
                g_deferred.head = (g_deferred.head + 1U) % DEFERRED_QUEUE_CAPACITY;
                --g_deferred.count;
                have_item = item.function != 0;
            }
            deferred_unlock(flags);
        }
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
