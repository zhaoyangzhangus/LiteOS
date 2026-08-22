#include <kernel/irq.h>
#include <kernel/spinlock.h>
#include <arch/x86_64/apic.h>
#include <arch/x86_64/context.h>

typedef struct irq_slot {
    /*
     * Registration is rare, dispatch is hot.  Writers serialize through the
     * global registration lock and publish a versioned handler/context pair.
     * Dispatch never takes that lock: if it observes a slot while a writer is
     * changing it, that one interrupt is treated as unhandled instead of
     * spinning in interrupt context.
     */
    atomic_uint sequence;
    _Atomic(irq_handler_t) handler;
    _Atomic(void *) context;
} irq_slot_t;

static spinlock_t g_irq_lock;
static atomic_uint g_irq_initialized;
static irq_slot_t g_irq_slots[IRQ_VECTOR_LAST + 1U];

static void irq_initialize(void) {
    unsigned expected = 0U;
    if (atomic_compare_exchange_strong_explicit(&g_irq_initialized, &expected, 1U,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        atomic_init(&g_irq_lock.state, 0U);
        for (uint32_t i = 0; i <= IRQ_VECTOR_LAST; ++i) {
            atomic_init(&g_irq_slots[i].sequence, 0U);
            atomic_init(&g_irq_slots[i].handler, (irq_handler_t)0);
            atomic_init(&g_irq_slots[i].context, (void *)0);
        }
        atomic_store_explicit(&g_irq_initialized, 2U, memory_order_release);
        return;
    }
    while (atomic_load_explicit(&g_irq_initialized, memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
}

static void irq_lock(void) {
    while (atomic_exchange_explicit(&g_irq_lock.state, 1U,
                                    memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
}

static void irq_unlock(void) {
    atomic_store_explicit(&g_irq_lock.state, 0U, memory_order_release);
}

static void irq_slot_write_begin(irq_slot_t *slot) {
    /* even -> odd.  acq_rel keeps the following pair update after this mark. */
    (void)atomic_fetch_add_explicit(&slot->sequence, 1U,
                                    memory_order_acq_rel);
}

static void irq_slot_write_end(irq_slot_t *slot) {
    /* odd -> even and publish the complete handler/context pair. */
    (void)atomic_fetch_add_explicit(&slot->sequence, 1U,
                                    memory_order_release);
}

kstatus_t irq_register(uint8_t vector, irq_handler_t handler, void *context) {
    if (vector < IRQ_VECTOR_FIRST || vector > IRQ_VECTOR_LAST || handler == 0) {
        return K_EINVAL;
    }
    irq_initialize();
    irq_lock();
    irq_slot_t *slot = &g_irq_slots[vector];
    if (atomic_load_explicit(&slot->handler, memory_order_acquire) != 0) {
        irq_unlock();
        return K_EBUSY;
    }
    irq_slot_write_begin(slot);
    atomic_store_explicit(&slot->context, context, memory_order_relaxed);
    atomic_store_explicit(&slot->handler, handler, memory_order_relaxed);
    irq_slot_write_end(slot);
    irq_unlock();
    return K_OK;
}

kstatus_t irq_unregister(uint8_t vector, irq_handler_t handler, void *context) {
    if (vector < IRQ_VECTOR_FIRST || vector > IRQ_VECTOR_LAST || handler == 0) {
        return K_EINVAL;
    }
    irq_initialize();
    irq_lock();
    irq_slot_t *slot = &g_irq_slots[vector];
    irq_handler_t current_handler =
        atomic_load_explicit(&slot->handler, memory_order_acquire);
    void *current_context =
        atomic_load_explicit(&slot->context, memory_order_relaxed);
    if (current_handler != handler || current_context != context) {
        irq_unlock();
        return K_ENOENT;
    }
    irq_slot_write_begin(slot);
    atomic_store_explicit(&slot->handler, (irq_handler_t)0,
                          memory_order_relaxed);
    atomic_store_explicit(&slot->context, (void *)0, memory_order_relaxed);
    irq_slot_write_end(slot);
    irq_unlock();
    return K_OK;
}

void x86_irq_dispatch(struct arch_trap_frame *frame) {
    irq_handler_t handler = 0;
    void *context = 0;

    if (frame == 0 || frame->vector < IRQ_VECTOR_FIRST ||
        frame->vector > IRQ_VECTOR_LAST) {
        liteos_lapic_end_of_interrupt();
        return;
    }
    irq_initialize();

    uint8_t vector = (uint8_t)frame->vector;
    irq_slot_t *slot = &g_irq_slots[vector];
    unsigned begin = atomic_load_explicit(&slot->sequence,
                                          memory_order_acquire);

    /*
     * Never spin on a writer from interrupt context.  The device must be
     * masked while its binding is changed, so seeing an odd sequence is an
     * exceptional reconfiguration race and dropping that edge is safer than
     * deadlocking the CPU that was interrupted while holding g_irq_lock.
     */
    if ((begin & 1U) == 0U) {
        handler = atomic_load_explicit(&slot->handler, memory_order_relaxed);
        context = atomic_load_explicit(&slot->context, memory_order_relaxed);
        unsigned end = atomic_load_explicit(&slot->sequence,
                                            memory_order_acquire);
        if (begin != end || (end & 1U) != 0U) {
            handler = 0;
            context = 0;
        }
    }

    if (handler != 0) handler(vector, frame, context);
    liteos_lapic_end_of_interrupt();
}

static void irq_self_test_handler(uint8_t vector, struct arch_trap_frame *frame,
                                  void *context) {
    (void)vector;
    (void)frame;
    uint32_t *hits = (uint32_t *)context;
    if (hits != 0) ++*hits;
}

bool irq_core_self_test(void) {
    uint32_t hits = 0;
    if (irq_register(48U, irq_self_test_handler, &hits) != K_OK ||
        irq_register(48U, irq_self_test_handler, &hits) != K_EBUSY) return false;
    struct arch_trap_frame frame = {0};
    frame.vector = 48U;
    x86_irq_dispatch(&frame);
    if (hits != 1U || irq_unregister(48U, irq_self_test_handler, &hits) != K_OK ||
        irq_unregister(48U, irq_self_test_handler, &hits) != K_ENOENT) return false;
    return true;
}
