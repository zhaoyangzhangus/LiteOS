#pragma once
#pragma once
#include <kernel/base.h>
#include <arch/x86_64/context.h>

enum {
    X86_IST_DOUBLE_FAULT = 1,
    X86_IST_NMI = 2,
    X86_IST_MACHINE_CHECK = 3,
};

void x86_idt_init(void);
void x86_apic_init(void);
void x86_ioapic_init(void);
void x86_interrupt_dispatch(arch_trap_frame_t *frame);

/* 触发真实 #BP/#UD，验证异常分类、陷阱帧和 IRET 恢复路径。 */
bool x86_exception_self_test(void);
