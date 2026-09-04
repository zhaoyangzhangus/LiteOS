#include "nv_port.h"

#include <arch/x86_64/context.h>
#include <kernel/irq.h>

#ifndef NV_PORT_HOST_TEST
#include <arch/x86_64/cpu.h>
#else
#include <stdlib.h>
#endif

#define NV_PORT_IRQ_VECTOR_COUNT (IRQ_VECTOR_LAST - IRQ_VECTOR_FIRST + 1U)

static atomic_uint g_nv_port_irq_init_state;
static nv_port_spinlock_t g_nv_port_irq_lock;
static uint64_t g_nv_port_irq_vectors[(NV_PORT_IRQ_VECTOR_COUNT + 63U) / 64U];

static void nv_port_irq_initialize(void) {
    unsigned expected = 0U;
    if (atomic_compare_exchange_strong_explicit(
            &g_nv_port_irq_init_state, &expected, 1U, memory_order_acq_rel,
            memory_order_acquire)) {
        nv_port_spin_init(&g_nv_port_irq_lock);
        for (uint32_t index = 0U;
             index < (NV_PORT_IRQ_VECTOR_COUNT + 63U) / 64U; ++index) {
            g_nv_port_irq_vectors[index] = 0U;
        }
        atomic_store_explicit(&g_nv_port_irq_init_state, 2U,
                              memory_order_release);
        return;
    }
    while (atomic_load_explicit(&g_nv_port_irq_init_state,
                                memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
}

static uint8_t nv_port_irq_allocate_vector(void) {
    nv_port_irq_initialize();
    uint64_t flags = nv_port_spin_lock_irqsave(&g_nv_port_irq_lock);
    uint8_t vector = 0U;
    for (uint32_t index = 0U; index < NV_PORT_IRQ_VECTOR_COUNT; ++index) {
        uint32_t word = index >> 6;
        uint64_t bit = 1ULL << (index & 63U);
        if ((g_nv_port_irq_vectors[word] & bit) == 0U) {
            g_nv_port_irq_vectors[word] |= bit;
            vector = (uint8_t)(IRQ_VECTOR_FIRST + index);
            break;
        }
    }
    nv_port_spin_unlock_irqrestore(&g_nv_port_irq_lock, flags);
    return vector;
}

static void nv_port_irq_free_vector(uint8_t vector) {
    if (vector < IRQ_VECTOR_FIRST || vector > IRQ_VECTOR_LAST) return;
    uint32_t index = vector - IRQ_VECTOR_FIRST;
    uint64_t flags = nv_port_spin_lock_irqsave(&g_nv_port_irq_lock);
    g_nv_port_irq_vectors[index >> 6] &= ~(1ULL << (index & 63U));
    nv_port_spin_unlock_irqrestore(&g_nv_port_irq_lock, flags);
}

static void nv_port_irq_handler(uint8_t vector, struct arch_trap_frame *frame,
                                void *context) {
    nv_port_irq_t *line = (nv_port_irq_t *)context;
    (void)vector;
    (void)frame;
    if (line == 0 || atomic_load_explicit(&line->stopping,
                                          memory_order_acquire)) return;
    atomic_fetch_add_explicit(&line->in_flight, 1U, memory_order_acq_rel);
    if (!atomic_load_explicit(&line->stopping, memory_order_acquire)) {
        if (line->top != 0) line->top(line->top_context);
        if (line->work.function != 0) (void)nv_port_work_queue_irq(&line->work);
    }
    atomic_fetch_sub_explicit(&line->in_flight, 1U, memory_order_release);
}

kstatus_t nv_port_irq_init(nv_port_irq_t *line, const pci_device_t *device,
                           bool use_msix, uint16_t msix_entry,
                           uint32_t apic_id, nv_port_irq_top_fn top,
                           void *top_context, nv_port_work_fn_t work,
                           void *work_argument) {
    if (line == 0 || (top == 0 && work == 0) || apic_id > 0xFFU) {
        return K_EINVAL;
    }
    *line = (nv_port_irq_t){0};
    line->device = device;
    line->msix_entry = msix_entry;
    line->apic_id = apic_id;
    line->use_msix = use_msix;
    line->top = top;
    line->top_context = top_context;
    atomic_init(&line->stopping, true);
    atomic_init(&line->in_flight, 0U);
    nv_port_work_init(&line->work, work, work_argument);
    return K_OK;
}

kstatus_t nv_port_irq_start(nv_port_irq_t *line) {
    if (line == 0 || line->registered || (line->top == 0 &&
                                           line->work.function == 0)) {
        return K_EINVAL;
    }
    uint8_t vector = nv_port_irq_allocate_vector();
    if (vector == 0U) return K_ENOSPC;
    line->vector = vector;
    atomic_store_explicit(&line->stopping, true, memory_order_release);
#ifdef NV_PORT_HOST_TEST
    line->registered = true;
    atomic_store_explicit(&line->stopping, false, memory_order_release);
    return K_OK;
#else
    if (line->device == 0) {
        nv_port_irq_free_vector(vector);
        line->vector = 0U;
        return K_EINVAL;
    }
    bool irq_registered = false;
    kstatus_t status = irq_register(vector, nv_port_irq_handler, line);
    if (status == K_OK) irq_registered = true;
    if (status == K_OK) {
        status = line->use_msix ?
            pci_msix_configure((pci_device_t *)line->device,
                               line->msix_entry, line->apic_id, vector) :
            pci_msi_configure((pci_device_t *)line->device,
                              line->apic_id, vector);
    }
    if (status != K_OK) {
        if (irq_registered) {
            (void)irq_unregister(vector, nv_port_irq_handler, line);
        }
        nv_port_irq_free_vector(vector);
        line->vector = 0U;
        return status;
    }
    line->registered = true;
    atomic_store_explicit(&line->stopping, false, memory_order_release);
    return K_OK;
#endif
}

kstatus_t nv_port_irq_stop(nv_port_irq_t *line) {
    if (line == 0 || !line->registered || line->vector == 0U) return K_EINVAL;
    atomic_store_explicit(&line->stopping, true, memory_order_release);
    kstatus_t status = K_OK;
#ifndef NV_PORT_HOST_TEST
    if (line->device != 0) {
        kstatus_t disable = line->use_msix ?
            pci_msix_mask((pci_device_t *)line->device, line->msix_entry, true) :
            pci_msi_disable((pci_device_t *)line->device);
        if (disable != K_OK) status = disable;
        kstatus_t unregister = irq_unregister(line->vector, nv_port_irq_handler,
                                               line);
        if (unregister != K_OK && status == K_OK) status = unregister;
    }
#endif
    while (atomic_load_explicit(&line->in_flight, memory_order_acquire) != 0U) {
#ifdef NV_PORT_HOST_TEST
        thrd_yield();
#else
        __asm__ volatile ("pause");
#endif
    }
    kstatus_t cancel = nv_port_work_cancel(&line->work);
    if (cancel == K_EBUSY) {
        kstatus_t drain = nv_port_work_drain(&line->work, UINT64_MAX);
        if (drain != K_OK && status == K_OK) status = drain;
    } else if (cancel != K_OK && cancel != K_EINVAL && status == K_OK) {
        status = cancel;
    }
    nv_port_irq_free_vector(line->vector);
    line->vector = 0U;
    line->registered = false;
    return status;
}

#ifdef NV_PORT_HOST_TEST
kstatus_t nv_port_irq_synthetic_fire(nv_port_irq_t *line) {
    if (line == 0 || !line->registered || line->vector == 0U) return K_EINVAL;
    nv_port_irq_handler(line->vector, 0, line);
    return K_OK;
}
#endif

static void nv_port_irq_test_top(void *context) {
    atomic_uint *hits = (atomic_uint *)context;
    if (hits != 0) atomic_fetch_add_explicit(hits, 1U, memory_order_relaxed);
}

bool nv_port_irq_self_test(void) {
#ifdef NV_PORT_HOST_TEST
    nv_port_irq_t line;
    atomic_uint hits;
    atomic_init(&hits, 0U);
    if (nv_port_irq_init(&line, 0, false, 0U, 0U, nv_port_irq_test_top,
                         &hits, 0, 0) != K_OK ||
        nv_port_irq_start(&line) != K_OK ||
        nv_port_irq_synthetic_fire(&line) != K_OK ||
        atomic_load_explicit(&hits, memory_order_acquire) != 1U ||
        nv_port_irq_stop(&line) != K_OK) return false;
    return nv_port_irq_synthetic_fire(&line) == K_EINVAL;
#else
    return true;
#endif
}

bool nv_port_irq_vector_stress_self_test(void) {
#ifdef NV_PORT_HOST_TEST
    enum { TEST_LINES = NV_PORT_IRQ_VECTOR_COUNT };
    nv_port_irq_t lines[TEST_LINES];
    atomic_uint hits;
    atomic_init(&hits, 0U);
    for (uint32_t index = 0U; index < TEST_LINES; ++index) {
        if (nv_port_irq_init(&lines[index], 0, false, 0U, 0U,
                             nv_port_irq_test_top, &hits, 0, 0) != K_OK ||
            nv_port_irq_start(&lines[index]) != K_OK) {
            for (uint32_t release = 0U; release < index; ++release) {
                (void)nv_port_irq_stop(&lines[release]);
            }
            return false;
        }
    }
    for (uint32_t index = 0U; index < TEST_LINES; ++index) {
        for (uint32_t round = 0U; round < 100U; ++round) {
            if (nv_port_irq_synthetic_fire(&lines[index]) != K_OK) {
                for (uint32_t release = 0U; release < TEST_LINES; ++release) {
                    (void)nv_port_irq_stop(&lines[release]);
                }
                return false;
            }
        }
    }
    bool success = atomic_load_explicit(&hits, memory_order_acquire) ==
                   TEST_LINES * 100U;
    for (uint32_t index = 0U; index < TEST_LINES; ++index) {
        if (nv_port_irq_stop(&lines[index]) != K_OK) success = false;
    }
    return success;
#else
    return true;
#endif
}
