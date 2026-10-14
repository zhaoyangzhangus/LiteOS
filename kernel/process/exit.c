#include <arch/x86_64/cpu.h>
#include <kernel/console.h>
#include <kernel/debug_stage.h>
#include <kernel/process.h>
#include <kernel/linux_abi.h>
#include <kernel/sched.h>
#include <kernel/vm.h>
#include "internal.h"

static atomic_flag g_process_exit_diag_lock = ATOMIC_FLAG_INIT;

static void process_exit_diag_char(char value) {
    for (uint32_t attempt = 0U; attempt < 100000U; ++attempt) {
        uint8_t ready;
        __asm__ volatile ("inb %w1, %0" : "=a"(ready) : "d"(0x3FDU));
        if ((ready & 0x20U) != 0U) break;
        __asm__ volatile ("pause");
    }
    __asm__ volatile ("outb %0, %w1" : : "a"((uint8_t)value), "d"(0x3F8U));
}

static void process_exit_diag_text(const char *text) {
    while (*text != '\0') process_exit_diag_char(*text++);
}

static void process_exit_diag_hex(uint64_t value) {
    static const char digits[] = "0123456789abcdef";
    process_exit_diag_text("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        process_exit_diag_char(digits[(value >> (uint32_t)shift) & 0xFU]);
    }
}

static void process_exit_diag(process_t *process, thread_t *thread,
                              int64_t status, uint32_t signal) {
    while (atomic_flag_test_and_set_explicit(&g_process_exit_diag_lock,
                                              memory_order_acquire)) {
        __asm__ volatile ("pause");
    }
    process_exit_diag_text("OS_PROCESS_EXIT pid=");
    process_exit_diag_hex(process != 0 ? process->pid : 0U);
    process_exit_diag_text(" tid=");
    process_exit_diag_hex(thread != 0 ? thread->tid : 0U);
    process_exit_diag_text(" status=");
    process_exit_diag_hex((uint64_t)status);
    process_exit_diag_text(" signal=");
    process_exit_diag_hex(signal);
    process_exit_diag_text("\r\n");
    atomic_flag_clear_explicit(&g_process_exit_diag_lock, memory_order_release);
}

static bool process_is_chrome_image(const process_t *process) {
    static const char path[] = "/sbin/chrome";
    if (process == 0) return false;
    for (size_t index = 0U; index < sizeof(path); ++index) {
        if (process->linux_exe[index] != path[index]) return false;
    }
    return true;
}

bool process_publish_thread_exit(thread_t *thread, int64_t status) {
    process_t *process;
    bool published;
    int64_t previous_status;
    bool remote_owner;

    if (thread == 0) return false;
    process = thread->process;
    remote_owner = thread->owner_cpu != x86_current_cpu_index();

    /* A remote scheduler acknowledgement may be deferred until syscall exit.
     * Never hold the process thread lock across that wait: the target CPU can
     * itself be trying to acquire this lock before it reaches that boundary.
     */
    if (process != 0 && !remote_owner) {
        process_thread_lock(process);
        /* Publish the result before making a local thread visibly dead. */
        previous_status = thread->exit_code;
        thread->exit_code = status;
    }
    published = sched_publish_dead(thread);
    if (process != 0 && !remote_owner) {
        /* A non-current or already-terminal thread did not publish this status. */
        if (!published) thread->exit_code = previous_status;
        process_thread_unlock(process);
    } else if (process != 0 && published) {
        /* Remote publication is complete; commit status under the owner lock. */
        process_thread_lock(process);
        thread->exit_code = status;
        process_thread_unlock(process);
    }
    if (!published) {
        thread_t *current = sched_current_thread();
        kernel_serial_write("OS_DIAG_THREAD_EXIT_PUBLISH_FAIL THREAD=");
        kernel_serial_write_u32((uint32_t)thread->tid);
        kernel_serial_write(" STATE=");
        kernel_serial_write_u32(atomic_load_explicit(&thread->state,
                                                     memory_order_acquire));
        kernel_serial_write(" CURRENT=");
        kernel_serial_write_u32(current != 0 ? (uint32_t)current->tid : 0U);
        kernel_serial_write(" PROCESS=");
        kernel_serial_write_u32(process != 0 ?
                                atomic_load_explicit(&process->state,
                                                     memory_order_acquire) : 0U);
        kernel_serial_write(" THREADS=");
        kernel_serial_write_u32(process != 0 ? process->thread_count : 0U);
        kernel_serial_write(" LOCK=");
        kernel_serial_write_u32(process != 0 ?
                                atomic_load_explicit(&process->thread_lock.state,
                                                     memory_order_acquire) : 0U);
        kernel_serial_write("\r\n");
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
     * is inactive. Reclaim the user stack while retaining its VM against
     * concurrent final-sibling resource teardown. */
    /* Stack registration uses the same owner lock. A reaper can claim its
     * execution flag while registration holds the lock, but cannot inspect
     * or clear the stack ownership until that transaction is complete. */
    if (process != 0) process_thread_lock(process);
    bool owned = thread->user_stack_owned;
    vaddr_t base = thread->user_stack_base;
    size_t size = thread->user_stack_size;
    thread->user_stack_owned = false;
    thread->user_stack_base = 0;
    thread->user_stack_size = 0;
    vm_space_t *space = owned && process != 0 ? process->vm : 0;
    if (space != 0) vm_space_get(space);
    if (process != 0) process_thread_unlock(process);
    if (space != 0) {
        (void)vm_unmap(space, base, size);
        vm_space_put(space);
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
        bool first_exit = process_begin_exit(process, status, signal);
        if (first_exit) process_exit_diag(process, thread, status, signal);

        /*
         * A sibling may be running on another CPU, in which case
         * thread_terminate() cannot reclaim it synchronously.  Mark every
         * sibling first so it exits at a safe user boundary; otherwise a
         * busy sibling can leave the process permanently EXITING while
         * retaining process resources.
         */
        if (first_exit) {
            process_terminate_siblings(process, thread, status);
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
        process_t *process = thread->process;
        if (process_is_chrome_image(process) &&
            (status != 0 || process->thread_count <= 1U)) {
            (void)kernel_serial_printf_serial_only(
                "OS_CHROME_FINAL_THREAD_EXIT pid=%llx tid=%llx "
                "status=%llx threads=%u\r\n",
                (unsigned long long)process->pid,
                (unsigned long long)thread->tid,
                (unsigned long long)(uint64_t)status,
                process->thread_count);
        }
        linux_thread_exit_cleanup(thread);
        /*
         * THREAD_DEAD becomes visible before the current context switches
         * away.  Keep the list detach and handle notification in that same
         * publication transaction; otherwise a timer can expose a dead
         * thread while process->thread_count still includes it.
         */
        sched_preempt_disable();
        debug_trace_stage(OS_DEBUG_PHASE_USER_RUNTIME,
                                 OS_DEBUG_STEP_THREAD_EXIT_ENTER,
                                 (uint32_t)thread->tid);
        if (!process_publish_thread_exit(thread, status)) {
            sched_preempt_enable();
            kernel_debug_stage_fail(OS_DEBUG_PHASE_USER_RUNTIME,
                                     OS_DEBUG_STEP_THREAD_EXIT_PUBLISHED,
                                     K_EBUSY);
            for (;;) __asm__ volatile ("cli; hlt" : : : "memory");
        }
        debug_trace_stage(OS_DEBUG_PHASE_USER_RUNTIME,
                                 OS_DEBUG_STEP_THREAD_EXIT_PUBLISHED,
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
    debug_trace_stage(OS_DEBUG_PHASE_USER_RUNTIME,
                             OS_DEBUG_STEP_THREAD_EXIT_SCHEDULE,
                             thread != 0 ? (uint32_t)thread->tid : 0U);
    schedule();
    for (;;) {
        __asm__ volatile ("sti; hlt" : : : "memory");
        schedule();
    }
}
