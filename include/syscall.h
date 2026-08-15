#ifndef LITEOS_SYSCALL_H
#define LITEOS_SYSCALL_H

#include <arch/x86_64/context.h>
#include <arch/x86_64/cpu.h>
#include <kernel/base.h>
#include <uapi/syscall.h>
#include "uefi.h"

/*
 * BSP 的早期每 CPU 数据。汇编使用 GS 相对地址访问，字段偏移属于架构 ABI，
 * 因此由下方静态断言固定。SMP 初始化后每个 CPU 都会拥有独立实例。
 */
typedef x86_cpu_local_t LITEOS_SYSCALL_CPU_LOCAL;

typedef arch_trap_frame_t LITEOS_SYSCALL_FRAME;

_Static_assert(__builtin_offsetof(LITEOS_SYSCALL_CPU_LOCAL, UserStack) == 0U,
               "syscall cpu-local UserStack offset");
_Static_assert(__builtin_offsetof(LITEOS_SYSCALL_CPU_LOCAL, KernelStack) == 8U,
               "syscall cpu-local KernelStack offset");
_Static_assert(__builtin_offsetof(LITEOS_SYSCALL_CPU_LOCAL, KernelResumeStack) == 16U,
               "syscall cpu-local KernelResumeStack offset");
_Static_assert(__builtin_offsetof(LITEOS_SYSCALL_CPU_LOCAL, ReturnToKernel) == 24U,
               "syscall cpu-local ReturnToKernel offset");
_Static_assert(__builtin_offsetof(LITEOS_SYSCALL_CPU_LOCAL, Self) == 40U,
               "cpu-local Self offset");
_Static_assert(__builtin_offsetof(LITEOS_SYSCALL_CPU_LOCAL, CpuIndex) == 48U,
               "cpu-local CpuIndex offset");
_Static_assert(__builtin_offsetof(LITEOS_SYSCALL_CPU_LOCAL, UserEntries) == 64U,
               "cpu-local UserEntries offset");

extern LITEOS_SYSCALL_CPU_LOCAL liteos_syscall_cpu_local;
extern void liteos_syscall_entry(void);
extern void liteos_arch_enter_user(uint64_t instruction_pointer, uint64_t stack_pointer);

BOOLEAN liteos_syscall_init(uint64_t kernel_stack_top);
void x86_syscall_set_kernel_stack(uint64_t kernel_stack_top);
int64_t liteos_syscall_dispatch(arch_trap_frame_t *frame);
uint32_t liteos_syscall_thread_create_stage(void);
int x86_syscall_return_mode(arch_trap_frame_t *frame);
bool x86_validate_user_frame(const arch_trap_frame_t *frame);
__noreturn void x86_syscall_bad_frame(void);
bool syscall_frame_self_test(void);
#endif
