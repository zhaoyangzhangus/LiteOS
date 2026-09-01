#include <arch/x86_64/cpu.h>
#include <kernel/console.h>
#include <kernel/debug_stage.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/vm.h>
#include "internal.h"

bool process_publish_thread_exit(thread_t *thread, int64_t status) {
    process_t *process;
    bool published;
    int64_t previous_status;

    if (thread == 0) return false;
    process = thread->process;
    if (process != 0) process_thread_lock(process);
    /* Process owns the status field; Scheduler only publishes THREAD_DEAD. */
    previous_status = thread->exit_code;
    thread->exit_code = status;
    published = sched_publish_dead(thread);
    /* A non-current or already-terminal thread did not publish this status. */
    if (!published) thread->exit_code = previous_status;
    if (process != 0) process_thread_unlock(process);
    if (!published) {
        thread_t *current = sched_current_thread();
        liteos_serial_write("LITEOS_DIAG_THREAD_EXIT_PUBLISH_FAIL THREAD=");
        liteos_serial_write_u32((uint32_t)thread->tid);
        liteos_serial_write(" STATE=");
        liteos_serial_write_u32(atomic_load_explicit(&thread->state,
                                                     memory_order_acquire));
        liteos_serial_write(" CURRENT=");
        liteos_serial_write_u32(current != 0 ? (uint32_t)current->tid : 0U);
        liteos_serial_write(" PROCESS=");
        liteos_serial_write_u32(process != 0 ?
                                atomic_load_explicit(&process->state,
                                                     memory_order_acquire) : 0U);
        liteos_serial_write(" THREADS=");
        liteos_serial_write_u32(process != 0 ? process->thread_count : 0U);
        liteos_serial_write(" LOCK=");
        liteos_serial_write_u32(process != 0 ?
                                atomic_load_explicit(&process->thread_lock.state,
                                                     memory_order_acquire) : 0U);
        liteos_serial_write("\r\n");
    }
    return published;
}

void thread_release_execution_ref(thread_t *thread) {
    if (thread == 0) return;
    uint32_t observed = __atomic_load_n(&thread->flags, __ATOMIC_ACQUIRE);
    for (;;) {
        if ((observed & THREAD_FLAG_EXECUTION_REF) == 0U ||
            (observed & THREAD_FLAG_EXECUTION_REAPING) != 0U) return;
        uint32_t desired = observed | THREAD_FLAG_EXECUTION_REAPING;
        if (__atomic_compare_exchange_n(&thread->flags, &observed, desired,
                                        false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            break;
        }
    }

    process_t *process = thread->process;
    /* The scheduler calls this only after the dead thread's execution stack
     * is inactive.  Reclaiming here also precedes process VM teardown when
     * this was the final thread, so pthread-owned stacks cannot leak. */
    if (thread->user_stack_owned) {
        vaddr_t base = thread->user_stack_base;
        size_t size = thread->user_stack_size;
        thread->user_stack_owned = false;
        thread->user_stack_base = 0;
        thread->user_stack_size = 0;
        if (process != 0 && process->vm != 0) {
            (void)vm_unmap(process->vm, base, size);
        }
    }
    if (process != 0 &&
        atomic_load_explicit(&process->state, memory_order_acquire) ==
        PROCESS_DEAD) {
        process_release_runtime_resources(process);
    }
    __atomic_fetch_and(&thread->flags,
                       ~(THREAD_FLAG_EXECUTION_REF |
                         THREAD_FLAG_EXECUTION_REAPING),
                       __ATOMIC_RELEASE);
    object_put(thread);
}

static __noreturn void process_exit_internal(int64_t status,
                                             uint32_t signal) {
    thread_t *thread = sched_current_thread();
    if (thread != 0 && thread->process != 0) {
        process_t *process = thread->process;
        process_thread_lock(process);
        if (atomic_load_explicit(&process->state, memory_order_acquire) ==
            PROCESS_RUNNING) {
            process->exit_code = status;
            process->exit_signal = signal;
            atomic_store_explicit(&process->state, PROCESS_EXITING,
                                  memory_order_release);
        }
        process_thread_unlock(process);

        /* Terminate siblings one at a time without holding the process lock. */
        for (;;) {
            thread_t *target = 0;
            process_thread_lock(process);
            for (list_head_t *node = process->threads.next;
                 node != &process->threads; node = node->next) {
                thread_t *candidate = (thread_t *)((uint8_t *)node -
                    __builtin_offsetof(thread_t, process_node));
                if (candidate != thread &&
                    atomic_load_explicit(&candidate->state,
                                         memory_order_acquire) != THREAD_DEAD) {
                    object_get(candidate);
                    target = candidate;
                    break;
                }
            }
            process_thread_unlock(process);
            if (target == 0) break;
            kstatus_t terminate_status = thread_terminate(target, status);
            object_put(target);
            if (terminate_status != K_OK) break;
        }
    }
    thread_exit(status);
}

__noreturn void process_exit(int64_t status) {
    process_exit_internal(status, 0U);
}

__noreturn void process_exit_signal(uint32_t signal) {
    process_exit_internal(-(int64_t)signal, signal);
}

__noreturn void thread_exit(int64_t status) {
    thread_t *thread = sched_current_thread();
    if (thread != 0) {
        /*
         * THREAD_DEAD becomes visible before the current context switches
         * away.  Keep the list detach and handle notification in that same
         * publication transaction; otherwise a timer can expose a dead
         * thread while process->thread_count still includes it.
         */
        sched_preempt_disable();
        liteos_debug_trace_stage(LITEOS_DEBUG_PHASE_USER_RUNTIME,
                                 LITEOS_DEBUG_STEP_THREAD_EXIT_ENTER,
                                 (uint32_t)thread->tid);
        if (!process_publish_thread_exit(thread, status)) {
            sched_preempt_enable();
            liteos_debug_stage_fail(LITEOS_DEBUG_PHASE_USER_RUNTIME,
                                     LITEOS_DEBUG_STEP_THREAD_EXIT_PUBLISHED,
                                     K_EBUSY);
            for (;;) __asm__ volatile ("cli; hlt" : : : "memory");
        }
        liteos_debug_trace_stage(LITEOS_DEBUG_PHASE_USER_RUNTIME,
                                 LITEOS_DEBUG_STEP_THREAD_EXIT_PUBLISHED,
                                 (uint32_t)status);
        process_detach_thread(thread);
        /*
         * Wake handle waiters only after the process list and any
         * PROCESS_DEAD transition are visible.  An execing sibling may
         * inspect thread_count immediately after WAIT_ONE returns.
         */
        object_notify_signaled(thread);
        sched_preempt_enable();
    }
    liteos_debug_trace_stage(LITEOS_DEBUG_PHASE_USER_RUNTIME,
                             LITEOS_DEBUG_STEP_THREAD_EXIT_SCHEDULE,
                             thread != 0 ? (uint32_t)thread->tid : 0U);
    schedule();
    for (;;) {
        __asm__ volatile ("sti; hlt" : : : "memory");
        schedule();
    }
}
