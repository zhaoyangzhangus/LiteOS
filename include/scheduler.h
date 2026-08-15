#ifndef LITEOS_SCHEDULER_H
#define LITEOS_SCHEDULER_H

#include "uefi.h"
#include "arch/x86_64/context.h"

#define LITEOS_SCHEDULER_PRIORITY_COUNT 32U

enum {
    LITEOS_THREAD_READY = 0U,
    LITEOS_THREAD_RUNNING,
    LITEOS_THREAD_BLOCKED,
    LITEOS_THREAD_SLEEPING,
    LITEOS_THREAD_EXITED,
    LITEOS_THREAD_TERMINATED = LITEOS_THREAD_EXITED,
};

#define LITEOS_THREAD_DEFAULT_QUANTUM 10U

struct LITEOS_CPU_CONTEXT {
    UINT64 InstructionPointer;
    UINT64 StackPointer;
    UINT64 Flags;
    UINT64 Registers[8];
};

typedef struct LITEOS_INTERRUPT_CONTEXT {
    UINT64 Rax;
    UINT64 Rcx;
    UINT64 Rdx;
    UINT64 Rbx;
    UINT64 Rbp;
    UINT64 Rsi;
    UINT64 Rdi;
    UINT64 R8;
    UINT64 R9;
    UINT64 R10;
    UINT64 R11;
    UINT64 R12;
    UINT64 R13;
    UINT64 R14;
    UINT64 R15;
    UINT64 InstructionPointer;
    UINT64 CodeSelector;
    UINT64 Flags;
    UINT64 StackPointer;
    UINT64 StackSelector;
} LITEOS_INTERRUPT_CONTEXT;

typedef struct LITEOS_THREAD {
    LITEOS_CPU_CONTEXT Context;
    UINT64 Identifier;
    UINT32 Priority;
    UINT32 State;
    UINT32 Quantum;
    UINT32 RemainingQuantum;
    UINT64 InterruptStackPointer;
    BOOLEAN InterruptContextValid;
    struct LITEOS_THREAD *Next;
    struct LITEOS_THREAD *Previous;
} LITEOS_THREAD;

typedef struct LITEOS_RUN_QUEUE {
    LITEOS_THREAD *Heads[LITEOS_SCHEDULER_PRIORITY_COUNT];
    LITEOS_THREAD *Tails[LITEOS_SCHEDULER_PRIORITY_COUNT];
    LITEOS_THREAD *Current;
    UINT32 ReadyBitmap;
    UINT32 ReadyCount;
} LITEOS_RUN_QUEUE;

VOID liteos_scheduler_init(LITEOS_RUN_QUEUE *queue);
BOOLEAN liteos_thread_init(LITEOS_THREAD *thread, UINT64 identifier, UINT32 priority);
BOOLEAN liteos_scheduler_set_current(LITEOS_RUN_QUEUE *queue, LITEOS_THREAD *thread);
BOOLEAN liteos_scheduler_enqueue(LITEOS_RUN_QUEUE *queue, LITEOS_THREAD *thread);
BOOLEAN liteos_scheduler_dequeue(LITEOS_RUN_QUEUE *queue, LITEOS_THREAD *thread);
LITEOS_THREAD *liteos_scheduler_pick_next(const LITEOS_RUN_QUEUE *queue);
BOOLEAN liteos_scheduler_tick(LITEOS_RUN_QUEUE *queue);

/* 在中断现场上执行一次可抢占调度，返回新现场地址；不切换时返回零。 */
UINT64 liteos_scheduler_timer_tick(LITEOS_RUN_QUEUE *queue,
                                    LITEOS_INTERRUPT_CONTEXT *context);

#endif
