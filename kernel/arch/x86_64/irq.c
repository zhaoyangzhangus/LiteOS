#include <kernel/irq.h>
#include <kernel/spinlock.h>
#include <arch/x86_64/apic.h>
#include <arch/x86_64/context.h>

typedef struct irq_slot {
    irq_handler_t handler;
    void *context;
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
            g_irq_slots[i].handler = 0;
            g_irq_slots[i].context = 0;
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

kstatus_t irq_register(uint8_t vector, irq_handler_t handler, void *context) {
    if (vector < IRQ_VECTOR_FIRST || vector > IRQ_VECTOR_LAST || handler == 0) {
        return K_EINVAL;
    }
    irq_initialize();
    irq_lock();
    irq_slot_t *slot = &g_irq_slots[vector];
    if (slot->handler != 0) {
        irq_unlock();
        return K_EBUSY;
    }
    slot->handler = handler;
    slot->context = context;
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
    if (slot->handler != handler || slot->context != context) {
        irq_unlock();
        return K_ENOENT;
    }
    slot->handler = 0;
    slot->context = 0;
    irq_unlock();
    return K_OK;
}

void x86_irq_dispatch(struct arch_trap_frame *frame) {
    if (frame == 0 || frame->vector < IRQ_VECTOR_FIRST ||
        frame->vector > IRQ_VECTOR_LAST) {
        liteos_lapic_end_of_interrupt();
        return;
    }
    irq_initialize();
    uint8_t vector = (uint8_t)frame->vector;
    irq_lock();
    irq_handler_t handler = g_irq_slots[vector].handler;
    void *context = g_irq_slots[vector].context;
    irq_unlock();
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
