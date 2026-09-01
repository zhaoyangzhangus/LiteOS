#pragma once
#pragma once
#include <kernel/base.h>
#include <arch/x86_64/context.h>

void x86_syscall_init(void);
__noreturn void x86_return_to_user(arch_trap_frame_t *frame);
