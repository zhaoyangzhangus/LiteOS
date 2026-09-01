#ifndef LITEOS_ARCH_X86_64_APIC_H
#define LITEOS_ARCH_X86_64_APIC_H

#include "uefi.h"

typedef struct LITEOS_INTERRUPT_CONTEXT LITEOS_INTERRUPT_CONTEXT;

#define LITEOS_LAPIC_BASE 0xFEE00000ULL

BOOLEAN liteos_lapic_init(UINT8 timer_vector, UINT32 initial_count);
BOOLEAN liteos_lapic_init_secondary(UINT32 expected_apic_id, UINT8 timer_vector,
                                    UINT32 initial_count);
VOID liteos_lapic_enable_timer(void);
BOOLEAN liteos_lapic_use_kernel_mapping(VOID);
VOID liteos_lapic_end_of_interrupt(void);
UINT64 liteos_lapic_tick_count(void);
UINT64 liteos_lapic_timer_interrupt(LITEOS_INTERRUPT_CONTEXT *context);
BOOLEAN liteos_lapic_send_init(UINT32 apic_id);
BOOLEAN liteos_lapic_send_init_deassert(UINT32 apic_id);
BOOLEAN liteos_lapic_send_startup(UINT32 apic_id, UINT8 vector);
BOOLEAN liteos_lapic_send_fixed(UINT32 apic_id, UINT8 vector);

#endif
