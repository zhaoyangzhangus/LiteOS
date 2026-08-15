#include "scheduler.h"

VOID liteos_scheduler_init(LITEOS_RUN_QUEUE *queue) {
    if (queue == 0) return;
    for (UINT32 priority = 0; priority < LITEOS_SCHEDULER_PRIORITY_COUNT; ++priority) {
        queue->Heads[priority] = 0;
        queue->Tails[priority] = 0;
    }
    queue->Current = 0;
    queue->ReadyBitmap = 0;
    queue->ReadyCount = 0;
}

BOOLEAN liteos_thread_init(LITEOS_THREAD *thread, UINT64 identifier, UINT32 priority) {
    if (thread == 0 || priority >= LITEOS_SCHEDULER_PRIORITY_COUNT) return 0;
    thread->Identifier = identifier;
    thread->Priority = priority;
    thread->State = LITEOS_THREAD_READY;
    thread->Quantum = LITEOS_THREAD_DEFAULT_QUANTUM;
    thread->RemainingQuantum = LITEOS_THREAD_DEFAULT_QUANTUM;
    thread->InterruptStackPointer = 0;
    thread->InterruptContextValid = 0;
    thread->Next = 0;
    thread->Previous = 0;
    thread->Context.InstructionPointer = 0;
    thread->Context.StackPointer = 0;
    thread->Context.Flags = 0x202ULL;
    for (UINT32 i = 0; i < 8; ++i) thread->Context.Registers[i] = 0;
    return 1;
}

BOOLEAN liteos_scheduler_set_current(LITEOS_RUN_QUEUE *queue, LITEOS_THREAD *thread) {
    if (queue == 0 || thread == 0 || queue->Current != 0 ||
        thread->State != LITEOS_THREAD_READY || thread->Next != 0 ||
        thread->Previous != 0) return 0;
    thread->State = LITEOS_THREAD_RUNNING;
    thread->RemainingQuantum = thread->Quantum;
    queue->Current = thread;
    return 1;
}

BOOLEAN liteos_scheduler_enqueue(LITEOS_RUN_QUEUE *queue, LITEOS_THREAD *thread) {
    if (queue == 0 || thread == 0 || thread->Priority >= LITEOS_SCHEDULER_PRIORITY_COUNT ||
        thread->State != LITEOS_THREAD_READY || thread->Next != 0 || thread->Previous != 0) return 0;
    UINT32 priority = thread->Priority;
    thread->Previous = queue->Tails[priority];
    thread->Next = 0;
    if (queue->Tails[priority] != 0) queue->Tails[priority]->Next = thread;
    else queue->Heads[priority] = thread;
    queue->Tails[priority] = thread;
    queue->ReadyBitmap |= 1U << priority;
    ++queue->ReadyCount;
    return 1;
}

BOOLEAN liteos_scheduler_dequeue(LITEOS_RUN_QUEUE *queue, LITEOS_THREAD *thread) {
    if (queue == 0 || thread == 0 || thread->Priority >= LITEOS_SCHEDULER_PRIORITY_COUNT) return 0;
    UINT32 priority = thread->Priority;
    if (thread->Previous != 0) thread->Previous->Next = thread->Next;
    else if (queue->Heads[priority] == thread) queue->Heads[priority] = thread->Next;
    else return 0;
    if (thread->Next != 0) thread->Next->Previous = thread->Previous;
    else if (queue->Tails[priority] == thread) queue->Tails[priority] = thread->Previous;
    else return 0;
    thread->Next = 0;
    thread->Previous = 0;
    if (queue->Heads[priority] == 0) queue->ReadyBitmap &= ~(1U << priority);
    if (queue->ReadyCount != 0) --queue->ReadyCount;
    return 1;
}

LITEOS_THREAD *liteos_scheduler_pick_next(const LITEOS_RUN_QUEUE *queue) {
    if (queue == 0 || queue->ReadyBitmap == 0) return 0;
    UINT32 priority = (UINT32)__builtin_ctz(queue->ReadyBitmap);
    return queue->Heads[priority];
}

BOOLEAN liteos_scheduler_tick(LITEOS_RUN_QUEUE *queue) {
    if (queue == 0 || queue->Current == 0) return 0;
    LITEOS_THREAD *current = queue->Current;
    if (current->RemainingQuantum > 1U) {
        --current->RemainingQuantum;
        return 0;
    }
    current->RemainingQuantum = current->Quantum;
    LITEOS_THREAD *next = liteos_scheduler_pick_next(queue);
    if (next == 0) return 0;
    current->State = LITEOS_THREAD_READY;
    if (!liteos_scheduler_enqueue(queue, current) ||
        !liteos_scheduler_dequeue(queue, next)) {
        liteos_scheduler_dequeue(queue, current);
        current->State = LITEOS_THREAD_RUNNING;
        return 0;
    }
    next->State = LITEOS_THREAD_RUNNING;
    next->RemainingQuantum = next->Quantum;
    queue->Current = next;
    liteos_arch_context_switch(&current->Context, &next->Context);
    return 1;
}

static LITEOS_THREAD *pick_interrupt_thread(const LITEOS_RUN_QUEUE *queue) {
    if (queue == 0) return 0;
    for (UINT32 priority = 0; priority < LITEOS_SCHEDULER_PRIORITY_COUNT; ++priority) {
        LITEOS_THREAD *thread = queue->Heads[priority];
        while (thread != 0) {
            if (thread->InterruptContextValid && thread->InterruptStackPointer != 0) {
                return thread;
            }
            thread = thread->Next;
        }
    }
    return 0;
}

UINT64 liteos_scheduler_timer_tick(LITEOS_RUN_QUEUE *queue,
                                    LITEOS_INTERRUPT_CONTEXT *context) {
    LITEOS_THREAD *current;
    LITEOS_THREAD *next;
    if (queue == 0 || context == 0 || queue->Current == 0) return 0;
    current = queue->Current;
    current->InterruptStackPointer = (UINT64)(uintptr_t)context;
    current->InterruptContextValid = 1;
    if (current->RemainingQuantum > 1U) {
        --current->RemainingQuantum;
        return 0;
    }
    next = pick_interrupt_thread(queue);
    current->RemainingQuantum = current->Quantum;
    if (next == 0) return 0;
    current->State = LITEOS_THREAD_READY;
    if (!liteos_scheduler_enqueue(queue, current) ||
        !liteos_scheduler_dequeue(queue, next)) {
        liteos_scheduler_dequeue(queue, current);
        current->State = LITEOS_THREAD_RUNNING;
        return 0;
    }
    next->State = LITEOS_THREAD_RUNNING;
    next->RemainingQuantum = next->Quantum;
    queue->Current = next;
    return next->InterruptStackPointer;
}
