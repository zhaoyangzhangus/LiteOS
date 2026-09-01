/* REFACTOR_SYSCALL_PROCESS_OWNER: process and thread syscall lifecycle. */

#include <arch/x86_64/cpu.h>
#include <arch/x86_64/paging.h>
#include <arch/x86_64/syscall_internal.h>
#include <arch/x86_64/uaccess.h>
#include <kernel/debug_stage.h>
#include <kernel/kmem.h>
#include <kernel/process.h>
#include <kernel/vm.h>
#include <uapi/process.h>
#include <uapi/signal.h>

#include "internal.h"

static volatile uint32_t g_thread_create_stage;

#define SIGNAL_FRAME_MAGIC 0x4c49544553494746ULL
#define SIGNAL_USER_MIN 0x0000000000010000ULL
#define SIGNAL_USER_END 0x0000800000000000ULL
#define SIGNAL_RED_ZONE_SIZE 128U

typedef struct user_signal_frame {
    uint64_t magic;
    uint64_t previous_frame;
    uint64_t old_mask;
    uint64_t reserved;
    arch_trap_frame_t context;
} user_signal_frame_t;

static uint64_t signal_bit(uint32_t signal) {
    return 1ULL << (signal - 1U);
}

static bool signal_number_valid(uint64_t signal) {
    return signal >= 1U && signal <= OS_SIGNAL_COUNT;
}

static bool signal_address_valid(uint64_t address) {
    return address >= SIGNAL_USER_MIN && address < SIGNAL_USER_END;
}

static bool signal_default_ignored(uint32_t signal) {
    return signal == OS_SIGCHLD || signal == OS_SIGCONT ||
           signal == OS_SIGURG || signal == OS_SIGWINCH;
}

static void signal_lock(process_t *process) {
    sched_preempt_disable();
    spinlock_lock(&process->signal_lock);
}

static void signal_unlock(process_t *process) {
    spinlock_unlock(&process->signal_lock);
    sched_preempt_enable();
}

static kstatus_t queue_current_signal(uint32_t signal) {
    thread_t *thread = sched_current_thread();
    if (thread == 0 || thread->process == 0) return K_EPERM;
    /* ponytail: target the caller until process-wide PID routing exists. */
    __atomic_fetch_or(&thread->signal_pending, signal_bit(signal),
                      __ATOMIC_RELEASE);
    return K_OK;
}

int64_t syscall_deliver_pending_signal(arch_trap_frame_t *frame,
                                       int64_t status) {
    thread_t *thread = sched_current_thread();
    process_t *process = thread != 0 ? thread->process : 0;
    if (frame == 0 || process == 0) return status;
    for (;;) {
        uint64_t pending = __atomic_load_n(&thread->signal_pending,
                                           __ATOMIC_ACQUIRE);
        uint64_t available = pending & ~thread->signal_mask;
        if (available == 0U) return status;
        uint32_t signal = (uint32_t)__builtin_ctzll(available) + 1U;
        uint64_t bit = signal_bit(signal);
        uint64_t desired = pending & ~bit;
        if (!__atomic_compare_exchange_n(&thread->signal_pending, &pending,
                                         desired, false, __ATOMIC_ACQ_REL,
                                         __ATOMIC_ACQUIRE)) {
            continue;
        }

        os_signal_action_t action;
        signal_lock(process);
        action = process->signal_actions[signal];
        if ((action.flags & OS_SIG_FLAG_RESETHAND) != 0U) {
            process->signal_actions[signal].handler = OS_SIG_DFL;
            process->signal_actions[signal].flags = 0U;
        }
        signal_unlock(process);
        if (action.handler == OS_SIG_IGN ||
            (action.handler == OS_SIG_DFL && signal_default_ignored(signal))) {
            continue;
        }
        if (action.handler == OS_SIG_DFL) process_exit_signal(signal);
        if (!signal_address_valid(action.handler) ||
            !signal_address_valid(action.restorer)) {
            process_exit_signal(OS_SIGSEGV);
        }

        user_signal_frame_t signal_frame = {
            .magic = SIGNAL_FRAME_MAGIC,
            .previous_frame = thread->signal_frame,
            .old_mask = thread->signal_mask,
            .reserved = 0U,
            .context = *frame,
        };
        signal_frame.context.rax = (uint64_t)status;
        uintptr_t frame_address = (frame->rsp - SIGNAL_RED_ZONE_SIZE -
                                   sizeof(signal_frame)) &
                                  ~(uintptr_t)15U;
        uintptr_t handler_stack = frame_address - sizeof(uint64_t);
        if (!signal_address_valid(handler_stack) ||
            copy_to_user((void __user *)frame_address, &signal_frame,
                         sizeof(signal_frame)) != K_OK ||
            copy_to_user((void __user *)handler_stack, &action.restorer,
                         sizeof(action.restorer)) != K_OK) {
            process_exit_signal(OS_SIGSEGV);
        }
        thread->signal_mask |= action.mask;
        if ((action.flags & OS_SIG_FLAG_NODEFER) == 0U) {
            thread->signal_mask |= bit;
        }
        /* SIGKILL and SIGSTOP may never be blocked. */
        thread->signal_mask &= ~OS_SIGNAL_UNBLOCKABLE_MASK;
        thread->signal_frame = frame_address;
        ++thread->signal_depth;
        frame->rip = action.handler;
        frame->rsp = handler_stack;
        frame->rdi = signal;
        frame->rsi = 0U;
        frame->rdx = 0U;
        frame->rax = 0U;
        return 0;
    }
}

int64_t syscall_signal_action(uint64_t signal, uint64_t action_pointer,
                              uint64_t old_pointer, uint64_t unused3,
                              uint64_t unused4, uint64_t unused5) {
    (void)unused3; (void)unused4; (void)unused5;
    process_t *process = current_process();
    if (process == 0) return K_EPERM;
    if (!signal_number_valid(signal)) return K_EINVAL;
    os_signal_action_t replacement;
    os_signal_action_t previous;
    if (action_pointer != 0U) {
        kstatus_t status = copy_from_user(
            &replacement, (const void __user *)(uintptr_t)action_pointer,
            sizeof(replacement));
        if (status != K_OK) return status;
        if (replacement.reserved != 0U ||
            (replacement.flags & ~(OS_SIG_FLAG_RESETHAND |
                                   OS_SIG_FLAG_NODEFER)) != 0U ||
            signal == OS_SIGKILL || signal == OS_SIGSTOP ||
            (replacement.handler != OS_SIG_DFL &&
             replacement.handler != OS_SIG_IGN &&
             (!signal_address_valid(replacement.handler) ||
              !signal_address_valid(replacement.restorer)))) {
            return K_EINVAL;
        }
    }
    signal_lock(process);
    previous = process->signal_actions[signal];
    if (action_pointer != 0U) process->signal_actions[signal] = replacement;
    signal_unlock(process);
    return old_pointer == 0U ? K_OK :
        copy_to_user((void __user *)(uintptr_t)old_pointer, &previous,
                     sizeof(previous));
}

int64_t syscall_signal_mask(uint64_t how, uint64_t set_pointer,
                            uint64_t old_pointer, uint64_t unused3,
                            uint64_t unused4, uint64_t unused5) {
    (void)unused3; (void)unused4; (void)unused5;
    thread_t *thread = sched_current_thread();
    if (thread == 0 || thread->process == 0) return K_EPERM;
    uint64_t old_mask = thread->signal_mask;
    uint64_t set = 0U;
    if (set_pointer != 0U) {
        kstatus_t status = copy_from_user(
            &set, (const void __user *)(uintptr_t)set_pointer, sizeof(set));
        if (status != K_OK) return status;
        if (how == OS_SIG_BLOCK) thread->signal_mask |= set;
        else if (how == OS_SIG_UNBLOCK) thread->signal_mask &= ~set;
        else if (how == OS_SIG_SETMASK) thread->signal_mask = set;
        else return K_EINVAL;
        thread->signal_mask &= ~OS_SIGNAL_UNBLOCKABLE_MASK;
    }
    return old_pointer == 0U ? K_OK :
        copy_to_user((void __user *)(uintptr_t)old_pointer, &old_mask,
                     sizeof(old_mask));
}

int64_t syscall_signal_send(uint64_t pid, uint64_t signal,
                            uint64_t unused2, uint64_t unused3,
                            uint64_t unused4, uint64_t unused5) {
    (void)unused2; (void)unused3; (void)unused4; (void)unused5;
    process_t *process = current_process();
    if (process == 0) return K_EPERM;
    if (pid != 0U && pid != process->pid) return K_ESRCH;
    if (signal == 0U) return K_OK;
    if (!signal_number_valid(signal)) return K_EINVAL;
    return queue_current_signal((uint32_t)signal);
}

int64_t syscall_signal_return(uint64_t frame_pointer, uint64_t unused1,
                              uint64_t unused2, uint64_t unused3,
                              uint64_t unused4, uint64_t unused5) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4; (void)unused5;
    thread_t *thread = sched_current_thread();
    if (thread == 0 || thread->process == 0 || thread->signal_depth == 0U ||
        frame_pointer != thread->signal_frame) return K_EPERM;
    user_signal_frame_t signal_frame;
    kstatus_t status = copy_from_user(
        &signal_frame, (const void __user *)(uintptr_t)frame_pointer,
        sizeof(signal_frame));
    if (status != K_OK || signal_frame.magic != SIGNAL_FRAME_MAGIC ||
        !x86_validate_user_frame(&signal_frame.context)) return K_EPERM;
    arch_trap_frame_t *active_frame = (arch_trap_frame_t *)(uintptr_t)unused5;
    if (active_frame == 0) return K_EPERM;
    *active_frame = signal_frame.context;
    thread->signal_mask = signal_frame.old_mask;
    thread->signal_frame = signal_frame.previous_frame;
    --thread->signal_depth;
    return (int64_t)active_frame->rax;
}

static int64_t thread_create_diag_fail(uint32_t step, kstatus_t status) {
    liteos_debug_stage_fail(LITEOS_DEBUG_PHASE_USER, (uint16_t)step, status);
    return status;
}

/* 句柄 0 表示当前进程；非零句柄必须拥有调用所需的进程权限。 */
static kstatus_t lookup_process(process_t *caller, handle_t handle, uint32_t rights,
                                process_t **out, bool *referenced) {
    if (caller == 0 || out == 0 || referenced == 0) return K_EINVAL;
    if (handle == OS_INVALID_HANDLE) {
        *out = caller;
        *referenced = false;
        return K_OK;
    }
    void *object = 0;
    kstatus_t status = handle_lookup(&caller->handles, handle, rights, &object);
    if (status != K_OK) return status;
    object_header_t *header = (object_header_t *)object;
    if (header->type != KOBJECT_TYPE_PROCESS) {
        object_put(object);
        return K_EINVAL;
    }
    *out = (process_t *)object;
    *referenced = true;
    return K_OK;
}

int64_t syscall_thread_exit(uint64_t status, uint64_t unused1, uint64_t unused2,
                               uint64_t unused3, uint64_t unused4, uint64_t unused5) {
    (void)status;
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    syscall_cpu_local_t *local = x86_cpu_local_current();
    if (local == 0) return K_EIO;
    local->UserExitSeen = 1U;
    process_exec_debug_mark(13U);
    if (local->KernelResumeStack != 0) {
        local->ReturnToKernel = 1U;
        return K_OK;
    }
    if (current_process() == 0) return K_EPERM;
    process_exec_debug_mark(14U);
    thread_exit((int64_t)status);
}

int64_t syscall_process_exit(uint64_t status, uint64_t unused1, uint64_t unused2,
                                uint64_t unused3, uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    if (current_process() == 0) return K_EPERM;
    process_exit((int64_t)status);
    return K_OK;
}

int64_t syscall_process_exec(uint64_t path, uint64_t argv, uint64_t envp,
                                uint64_t unused3, uint64_t unused4, uint64_t unused5) {
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0) return K_EPERM;
    os_exec_fd_map_t *descriptor_map = 0;
    if (unused3 != 0U) {
        descriptor_map = (os_exec_fd_map_t *)kmalloc(sizeof(*descriptor_map), 0);
        if (descriptor_map == 0) return K_ENOMEM;
        kstatus_t map_status = copy_from_user(
            descriptor_map, (const void __user *)(uintptr_t)unused3,
            sizeof(*descriptor_map));
        if (map_status != K_OK ||
            !versioned_header_valid(&descriptor_map->hdr,
                                     sizeof(*descriptor_map)) ||
            descriptor_map->reserved != 0U ||
            descriptor_map->count > OS_EXEC_FD_LIMIT) {
            kfree(descriptor_map);
            return map_status != K_OK ? map_status : K_EINVAL;
        }
    }
    int64_t status = process_exec(
        process, (const char __user *)(uintptr_t)path,
        (const char __user *const __user *)(uintptr_t)argv,
        (const char __user *const __user *)(uintptr_t)envp, descriptor_map);
    kfree(descriptor_map);
    return status;
}

int64_t syscall_process_info(uint64_t arguments_pointer, uint64_t unused1,
                             uint64_t unused2, uint64_t unused3,
                             uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    os_process_info_t info = {0};
    if (process == 0) return K_EPERM;
    info.hdr.size = sizeof(info);
    info.hdr.version = OS_SYSCALL_ABI_VERSION;
    info.pid = process->pid;
    info.parent_pid = process_parent_pid(process);
    info.state = atomic_load_explicit(&process->state, memory_order_acquire);
    info.exit_code = process->exit_code;
    return copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                        &info, sizeof(info));
}

int64_t syscall_process_enumerate(uint64_t arguments_pointer, uint64_t unused1,
                                  uint64_t unused2, uint64_t unused3,
                                  uint64_t unused4, uint64_t unused5) {
    os_process_enumerate_t arguments;
    kstatus_t status;
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    if (arguments_pointer == 0U) return K_EINVAL;
    status = copy_from_user(&arguments,
                            (const void __user *)(uintptr_t)arguments_pointer,
                            sizeof(arguments));
    if (status != K_OK ||
        !versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.reserved != 0U) {
        return status != K_OK ? status : K_EINVAL;
    }
    status = process_enumerate(arguments.index, &arguments.info);
    if (status != K_OK) return status;
    return copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                        &arguments, sizeof(arguments));
}

int64_t syscall_thread_enumerate(uint64_t arguments_pointer, uint64_t unused1,
                                 uint64_t unused2, uint64_t unused3,
                                 uint64_t unused4, uint64_t unused5) {
    os_thread_enumerate_t arguments;
    kstatus_t status;
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    if (arguments_pointer == 0U) return K_EINVAL;
    status = copy_from_user(&arguments,
                            (const void __user *)(uintptr_t)arguments_pointer,
                            sizeof(arguments));
    if (status != K_OK ||
        !versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.reserved != 0U) {
        return status != K_OK ? status : K_EINVAL;
    }
    status = process_enumerate_thread(arguments.index, &arguments.info);
    if (status != K_OK) return status;
    return copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                        &arguments, sizeof(arguments));
}

int64_t syscall_process_fork(uint64_t unused0, uint64_t unused1,
                             uint64_t unused2, uint64_t unused3,
                             uint64_t unused4, uint64_t frame_pointer) {
    (void)unused0;
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    process_t *parent = current_process();
    thread_t *current = sched_current_thread();
    arch_trap_frame_t *parent_frame =
        (arch_trap_frame_t *)(uintptr_t)frame_pointer;
    if (parent == 0 || current == 0 || parent_frame == 0 ||
        !x86_validate_user_frame(parent_frame)) return K_EPERM;

    process_t *child = 0;
    kstatus_t status = process_fork(parent, &child);
    if (status != K_OK) return status;

    arch_trap_frame_t child_frame = *parent_frame;
    child_frame.rax = 0;
    child_frame.vector = 0;
    child_frame.error_code = 0;
    thread_t *child_thread = 0;
    status = thread_create_user_from_frame(child, &child_frame,
                                            (vaddr_t)current->arch.fs_base,
                                            &child_thread);
    if (status == K_OK) {
        x86_fp_state_clone_current(&current->arch, &child_thread->arch);
        child_thread->signal_mask = current->signal_mask;
    }
    if (status == K_OK) status = thread_start(child_thread);
    if (status != K_OK) {
        if (child_thread != 0) {
            (void)thread_terminate(child_thread, K_ECANCELED);
            object_put(child_thread);
        }
        process_abort(child);
        object_put(child);
        return status;
    }

    uint64_t child_pid = child->pid;
    object_put(child_thread);
    object_put(child);
    return (int64_t)child_pid;
}

int64_t syscall_process_wait(uint64_t pid, uint64_t options,
                             uint64_t status_pointer, uint64_t unused3,
                             uint64_t unused4, uint64_t unused5) {
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *parent = current_process();
    int64_t child_pid = 0;
    int64_t exit_code = 0;
    uint32_t exit_signal = 0U;
    kstatus_t status;
    if (parent == 0 ||
        ((int64_t)pid <= 0 && (int64_t)pid != -1)) return K_EINVAL;
    status = process_wait_child(parent, (int64_t)pid, (uint32_t)options,
                                &child_pid, &exit_code, &exit_signal);
    if (status != K_OK || child_pid == 0 || status_pointer == 0) {
        if (status != K_OK) return status;
        return child_pid;
    }
    int32_t wait_status = exit_signal != 0U ? (int32_t)(exit_signal & 0x7FU) :
        (int32_t)(((uint64_t)exit_code & 0xFFULL) << 8);
    status = copy_to_user((void __user *)(uintptr_t)status_pointer,
                          &wait_status, sizeof(wait_status));
    return status == K_OK ? child_pid : status;
}

int64_t syscall_thread_context(uint64_t operation, uint64_t fs_base,
                               uint64_t unused2, uint64_t unused3,
                               uint64_t unused4, uint64_t unused5) {
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    thread_t *thread = sched_current_thread();
    if (thread == 0 || thread->process == 0) return K_EPERM;
    if (operation == OS_THREAD_CONTEXT_GET_FS) {
        return (int64_t)thread->arch.fs_base;
    }
    if (operation != OS_THREAD_CONTEXT_SET_FS ||
        !x86_is_canonical((vaddr_t)fs_base) || fs_base > X86_64_USER_TOP) {
        return K_EINVAL;
    }
    thread->arch.fs_base = (vaddr_t)fs_base;
    x86_set_user_fs_base((vaddr_t)fs_base);
    return K_OK;
}

/*
 * THREAD_CREATE(target_process, args, out_handle)：target_process 为 0 时创建
 * 当前进程线程。先写回句柄，最后才把线程发布到运行队列，失败路径不会留下孤儿线程。
 */
int64_t syscall_thread_create(uint64_t process_handle, uint64_t arguments_pointer,
                                 uint64_t output_pointer, uint64_t stack_pointer,
                                 uint64_t unused4, uint64_t unused5) {
    (void)unused4;
    (void)unused5;
    process_t *caller = current_process();
    g_thread_create_stage = 1U;
    if (caller == 0) {
        return thread_create_diag_fail(1U, K_EPERM);
    }
    os_thread_create_t arguments;
    kstatus_t status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    if (status != K_OK) {
        return thread_create_diag_fail(2U, status);
    }
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.flags != 0 || arguments.reserved != 0) {
        return thread_create_diag_fail(3U, K_EINVAL);
    }

    os_thread_stack_t stack_arguments = {0};
    bool owns_user_stack = false;
    if (stack_pointer != 0U) {
        status = copy_from_user(&stack_arguments,
            (const void __user *)(uintptr_t)stack_pointer,
            sizeof(stack_arguments));
        if (status != K_OK ||
            !versioned_header_valid(&stack_arguments.hdr,
                                     sizeof(stack_arguments)) ||
            (stack_arguments.flags & ~OS_THREAD_STACK_OWNED) != 0U ||
            stack_arguments.reserved != 0U) {
            return thread_create_diag_fail(3U, K_EINVAL);
        }
        owns_user_stack = (stack_arguments.flags & OS_THREAD_STACK_OWNED) != 0U;
        if (owns_user_stack &&
            (stack_arguments.base > UINT64_MAX - stack_arguments.size ||
             stack_arguments.size == 0U ||
             (stack_arguments.base & (PAGE_SIZE - 1ULL)) != 0U ||
             (stack_arguments.size & (PAGE_SIZE - 1ULL)) != 0U)) {
            return thread_create_diag_fail(3U, K_EINVAL);
        }
    }

    process_t *target = 0;
    bool target_referenced = false;
    status = lookup_process(caller, (handle_t)process_handle,
                            PROCESS_RIGHT_CREATE_THREAD, &target, &target_referenced);
    g_thread_create_stage = 2U;
    if (status != K_OK) {
        return thread_create_diag_fail(4U, status);
    }

    /* 创建前同时验证入口和栈权限；按需匿名页会在这里完成首次缺页。 */
    vm_fault_info_t entry_fault = {
        .address = (vaddr_t)arguments.entry,
        .access = VM_PROT_EXEC,
        .cpu_error = 0,
    };
    vm_fault_info_t stack_fault = {
        .address = arguments.stack_top >= sizeof(uint64_t) ?
                   (vaddr_t)(arguments.stack_top - sizeof(uint64_t)) : 0,
        .access = VM_PROT_WRITE,
        .cpu_error = 0,
    };
    status = vm_handle_fault(target->vm, &entry_fault);
    g_thread_create_stage = 3U;
    if (status != K_OK) {
        if (target_referenced) object_put(target);
        return thread_create_diag_fail(5U, status);
    }

    status = vm_handle_fault(target->vm, &stack_fault);
    g_thread_create_stage = 4U;
    if (status != K_OK) {
        if (target_referenced) object_put(target);
        return thread_create_diag_fail(6U, status);
    }

    thread_t *thread = 0;
    handle_t handle = 0;
    status = thread_create_user_suspended(target, (vaddr_t)arguments.entry,
                                           (vaddr_t)arguments.stack_top,
                                           (vaddr_t)arguments.fs_base,
                                           arguments.argument, &thread);
    thread_t *creator = sched_current_thread();
    if (status == K_OK && target == caller && creator != 0) {
        thread->signal_mask = creator->signal_mask;
    }
    if (status == K_OK && owns_user_stack) {
        uint64_t stack_end = stack_arguments.base + stack_arguments.size;
        if ((uint64_t)arguments.stack_top < stack_arguments.base ||
            (uint64_t)arguments.stack_top > stack_end ||
            thread_register_user_stack(thread,
                                       (vaddr_t)stack_arguments.base,
                                       (size_t)stack_arguments.size) != K_OK) {
            status = K_EINVAL;
        }
    }
    g_thread_create_stage = 5U;
    if (target_referenced) object_put(target);
    if (status != K_OK) {
        /* The suspended thread already owns an execution reference and may
         * also own the caller-provided stack.  Roll it back before exposing
         * the failure, otherwise the stack mapping and thread object leak. */
        if (thread != 0) {
            (void)thread_terminate(thread, K_ECANCELED);
            object_put(thread);
        }
        return thread_create_diag_fail(7U, status);
    }
    status = handle_create(&caller->handles, thread, THREAD_RIGHT_ALL, &handle);
    g_thread_create_stage = 6U;
    uint32_t failure_step = 8U;
    if (status == K_OK) {
        failure_step = 9U;
        status = copy_to_user((void __user *)(uintptr_t)output_pointer,
                              &handle, sizeof(handle));
    }
    if (status == K_OK) {
        failure_step = 10U;
        status = thread_start(thread);
        g_thread_create_stage = 7U;
    }
    if (status != K_OK) {
        (void)thread_create_diag_fail(failure_step, status);
    }
    if (status != K_OK && handle != 0) {
        (void)handle_close(&caller->handles, handle);
        handle = 0;
        /* 写回已成功但发布失败时，尽力把用户可见句柄清零。 */
        (void)copy_to_user((void __user *)(uintptr_t)output_pointer,
                           &handle, sizeof(handle));
    }
    if (status != K_OK) (void)thread_terminate(thread, K_ECANCELED);
    object_put(thread);
    return status;
}

/* PROCESS_CREATE(flags, out_handle) 当前提供带 COW 地址空间的空子进程。 */
static kstatus_t syscall_copy_process_name(uint64_t pointer, char *name) {
    if (pointer == 0U || name == 0) return K_EINVAL;
    for (size_t index = 0U; index + 1U < OS_PROCESS_NAME_MAX; ++index) {
        kstatus_t status = copy_from_user(
            &name[index],
            (const void __user *)(uintptr_t)(pointer + index),
            sizeof(name[index]));
        if (status != K_OK) return status;
        if (name[index] == '\0') return index != 0U ? K_OK : K_EINVAL;
    }
    name[OS_PROCESS_NAME_MAX - 1U] = '\0';
    return K_EINVAL;
}

int64_t syscall_process_create(uint64_t flags, uint64_t output_pointer,
                                  uint64_t name_pointer, uint64_t unused3,
                                  uint64_t unused4, uint64_t unused5) {
    char name[OS_PROCESS_NAME_MAX] = {0};
    const char *name_argument = 0;
    kstatus_t status;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    if (flags != 0 || output_pointer == 0U) return K_EINVAL;
    process_t *caller = current_process();
    if (caller == 0) return K_EPERM;
    if (name_pointer != 0U) {
        status = syscall_copy_process_name(name_pointer, name);
        if (status != K_OK) return status;
        name_argument = name;
    }
    process_t *child = 0;
    status = process_create_named(caller, name_argument, &child);
    if (status != K_OK) return status;
    handle_t handle = 0;
    status = handle_create(&caller->handles, child, PROCESS_RIGHT_ALL, &handle);
    if (status == K_OK) {
        status = copy_to_user((void __user *)(uintptr_t)output_pointer,
                              &handle, sizeof(handle));
    }
    if (status != K_OK && handle != 0) (void)handle_close(&caller->handles, handle);
    if (status != K_OK) (void)process_abort(child);
    object_put(child);
    return status;
}

uint32_t liteos_syscall_thread_create_stage(void) {
    return g_thread_create_stage;
}
