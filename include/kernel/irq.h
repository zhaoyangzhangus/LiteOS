#pragma once

#pragma once

#include "base.h"

struct arch_trap_frame;

typedef void (*irq_handler_t)(uint8_t vector, struct arch_trap_frame *frame,
                              void *context);

#define IRQ_VECTOR_FIRST 33U
#define IRQ_VECTOR_LAST  254U

kstatus_t irq_register(uint8_t vector, irq_handler_t handler, void *context);
kstatus_t irq_unregister(uint8_t vector, irq_handler_t handler, void *context);
void x86_irq_dispatch(struct arch_trap_frame *frame);
bool irq_core_self_test(void);
