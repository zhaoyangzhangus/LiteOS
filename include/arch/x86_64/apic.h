#ifndef OS_ARCH_X86_64_APIC_H
#define OS_ARCH_X86_64_APIC_H

#include "uefi.h"

typedef struct OS_INTERRUPT_CONTEXT OS_INTERRUPT_CONTEXT;

#define OS_LAPIC_BASE 0xFEE00000ULL

BOOLEAN lapic_init(UINT8 timer_vector, UINT32 initial_count);
BOOLEAN lapic_init_secondary(UINT32 expected_apic_id, UINT8 timer_vector,
                                    UINT32 initial_count);
VOID lapic_enable_timer(void);
BOOLEAN lapic_use_kernel_mapping(VOID);
VOID lapic_end_of_interrupt(void);
UINT64 lapic_tick_count(void);
UINT64 lapic_timer_interrupt(OS_INTERRUPT_CONTEXT *context);
BOOLEAN lapic_send_init(UINT32 apic_id);
BOOLEAN lapic_send_init_deassert(UINT32 apic_id);
BOOLEAN lapic_send_startup(UINT32 apic_id, UINT8 vector);
BOOLEAN lapic_send_fixed(UINT32 apic_id, UINT8 vector);

#endif
