#include <arch/x86_64/context.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/paging.h>
#include <kernel/elf_loader.h>
#include <kernel/io.h>
#include <kernel/process.h>
#include <kernel/resource.h>
#include <kernel/sched.h>
#include <kernel/security.h>
#include <kernel/wait.h>
#include <kernel/window_server.h>

#define USER_CODE_SELECTOR    0x23ULL
#define USER_DATA_SELECTOR    0x1BULL
#define USER_INITIAL_RFLAGS   0x202ULL
#define THREAD_KERNEL_STACK_SIZE (64U * 1024U)

static atomic_uint_fast64_t g_next_pid;
static atomic_uint_fast64_t g_next_tid;
static atomic_uint g_process_init_state;
static volatile uint32_t g_thread_create_stage;

extern void x86_user_thread_start(void);

static void process_destroy(void *object);
static void thread_destroy(void *object);
static bool process_is_signaled(const void *object);
static bool thread_is_signaled(const void *object);
static int64_t process_wait_value(const void *object);
static int64_t thread_wait_value(const void *object);
static void process_release_runtime_resources(process_t *process);

static const object_ops_t g_process_ops = {
    .destroy = process_destroy,
    .type_name = "Process",
    .is_signaled = process_is_signaled,
    .wait_value = process_wait_value,
};

static const object_ops_t g_thread_ops = {
    .destroy = thread_destroy,
    .type_name = "Thread",
    .is_signaled = thread_is_signaled,
    .wait_value = thread_wait_value,
};

static void process_lock(spinlock_t *lock) {
    while (atomic_exchange_explicit(&lock->state, 1U,
                                     memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
}

static void process_unlock(spinlock_t *lock) {
    atomic_store_explicit(&lock->state, 0U, memory_order_release);
}

static void list_insert_tail(list_head_t *head, list_head_t *node) {
    node->next = head;
    node->prev = head->prev;
    head->prev->next = node;
    head->prev = node;
}

static void list_remove_node(list_head_t *node) {
    node->prev->next = node->next;
    node->next->prev = node->prev;
    list_init(node);
}

static void thread_detach_from_process(thread_t *thread) {
    process_t *process = thread != 0 ? thread->process : 0;
    if (process == 0) return;
    bool notify_process = false;
    process_lock(&process->thread_lock);
    if (thread->process_node.next != &thread->process_node) {
        list_remove_node(&thread->process_node);
        if (process->thread_count != 0) --process->thread_count;
        if (process->thread_count == 0 &&
            (process->flags & PROCESS_FLAG_EVER_HAD_THREAD) != 0) {
            unsigned state = atomic_load_explicit(&process->state, memory_order_acquire);
            if (state == PROCESS_RUNNING) process->exit_code = thread->exit_code;
            if (state == PROCESS_RUNNING || state == PROCESS_EXITING) {
                atomic_store_explicit(&process->state, PROCESS_DEAD, memory_order_release);
                notify_process = true;
            }
        }
    }
    process_unlock(&process->thread_lock);
    if (notify_process) {
        /* 进程已无可运行线程，窗口必须先从合成列表消失，不能等到
         * 最后的 process 引用归零后才清理，否则旧画面会残留。 */
        window_server_close_process(process);
        io_cancel_process(process);
    }
    if (notify_process) object_notify_signaled(process);
}

static bool process_is_signaled(const void *object) {
    const process_t *process = (const process_t *)object;
    return atomic_load_explicit(&process->state, memory_order_acquire) == PROCESS_DEAD;
}

static bool thread_is_signaled(const void *object) {
    const thread_t *thread = (const thread_t *)object;
    return atomic_load_explicit(&thread->state, memory_order_acquire) == THREAD_DEAD;
}

static int64_t process_wait_value(const void *object) {
    return ((const process_t *)object)->exit_code;
}

static int64_t thread_wait_value(const void *object) {
    return ((const thread_t *)object)->exit_code;
}

static uint64_t read_tsc(void) {
    uint32_t low;
    uint32_t high;
    __asm__ volatile ("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32) | low;
}

static void process_global_initialize(void) {
    unsigned expected = 0U;
    if (atomic_compare_exchange_strong_explicit(&g_process_init_state, &expected, 1U,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        atomic_init(&g_next_pid, 1U);
        atomic_init(&g_next_tid, 1U);
        atomic_store_explicit(&g_process_init_state, 2U, memory_order_release);
        return;
    }
    while (atomic_load_explicit(&g_process_init_state, memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
}

static void initialize_object(object_header_t *header, object_type_id_t type,
                              const object_ops_t *ops) {
    refcount_init(&header->refs, 1U);
    header->type = type;
    header->flags = 0;
    header->ops = ops;
    header->security = 0;
}

kstatus_t process_create(process_t *parent, process_t **out) {
    if (out == 0) return K_EINVAL;
    process_global_initialize();
    process_t *process = (process_t *)kzalloc(sizeof(process_t), 0);
    if (process == 0) return K_ENOMEM;
    initialize_object(&process->object, KOBJECT_TYPE_PROCESS, &g_process_ops);
    process->pid = atomic_fetch_add_explicit(&g_next_pid, 1U, memory_order_relaxed);
    atomic_init(&process->state, PROCESS_NEW);
    if (parent != 0) {
        process->flags = parent->flags & PROCESS_FLAG_INIT_CPU_PINNED;
    }
    atomic_init(&process->thread_lock.state, 0U);
    list_init(&process->threads);
    list_init(&process->job_node);
    process->create_time_ns = read_tsc();

    kstatus_t status = parent != 0 ? vm_space_clone_cow(parent->vm, &process->vm) :
                                    vm_space_create(&process->vm);
    if (status != K_OK) {
        kfree(process);
        return status;
    }
    status = handle_table_init(&process->handles);
    if (status != K_OK) {
        vm_space_put(process->vm);
        kfree(process);
        return status;
    }
    process->parent = parent;
    if (parent != 0) {
        object_get(parent);
        process->token = parent->token;
        if (process->token != 0) object_get(process->token);
        process->session = parent->session;
        if (process->session != 0) object_get(process->session);
        status = parent->job != 0 ? job_attach_process(parent->job, process) : K_OK;
        if (status != K_OK) {
            if (process->session != 0) object_put(process->session);
            if (process->token != 0) object_put(process->token);
            object_put(parent);
            handle_table_destroy(&process->handles);
            vm_space_put(process->vm);
            kfree(process);
            return status;
        }
    } else {
        status = security_token_create(0U, 0U, 0U, 0U,
                                       SECURITY_CAPABILITY_SYSTEM_ADMIN,
                                       &process->token);
        if (status != K_OK) {
            handle_table_destroy(&process->handles);
            vm_space_put(process->vm);
            kfree(process);
            return status;
        }
        status = session_create(process->token, 0, &process->session);
        if (status == K_OK) {
            status = job_attach_process(process->session->job, process);
        }
        if (status != K_OK) {
            if (process->session != 0) object_put(process->session);
            if (process->token != 0) object_put(process->token);
            handle_table_destroy(&process->handles);
            vm_space_put(process->vm);
            kfree(process);
            return status;
        }
    }
    atomic_store_explicit(&process->state, PROCESS_RUNNING, memory_order_release);
    *out = process;
    return K_OK;
}

static void process_destroy(void *object) {
    process_t *process = (process_t *)object;
    atomic_store_explicit(&process->state, PROCESS_DEAD, memory_order_release);
    (void)job_detach_process(process);
    process_release_runtime_resources(process);
    if (process->session != 0) object_put(process->session);
    if (process->token != 0) object_put(process->token);
    if (process->parent != 0) object_put(process->parent);
    kfree(process);
}

static void process_release_runtime_resources(process_t *process) {
    if (process == 0) return;
    uint32_t old_flags = __atomic_fetch_or(&process->flags,
                                           PROCESS_FLAG_RESOURCES_RELEASED,
                                           __ATOMIC_ACQ_REL);
    if ((old_flags & PROCESS_FLAG_RESOURCES_RELEASED) != 0) return;
    handle_table_destroy(&process->handles);
    vm_space_t *space = process->vm;
    process->vm = 0;
    if (space != 0) vm_space_put(space);
}

static kstatus_t thread_create_user_internal(process_t *process, vaddr_t entry,
                                             vaddr_t stack_top, vaddr_t fs_base,
                                             uint64_t argument, bool start,
                                             thread_t **out) {
    if (process == 0 || out == 0 || !x86_is_canonical(entry) ||
        !x86_is_canonical(stack_top) || !x86_is_canonical(fs_base) ||
        entry < 0x10000ULL || entry > X86_64_USER_TOP ||
        stack_top < 0x10000ULL || stack_top > X86_64_USER_TOP ||
        fs_base > X86_64_USER_TOP ||
        (stack_top & 15U) != 0) {
        return K_EINVAL;
    }
    if (atomic_load_explicit(&process->state, memory_order_acquire) != PROCESS_RUNNING) {
        return K_EBUSY;
    }
    thread_t *thread = (thread_t *)kzalloc(sizeof(thread_t), 0);
    g_thread_create_stage = 1U;
    if (thread == 0) {
        return K_ENOMEM;
    }
    initialize_object(&thread->object, KOBJECT_TYPE_THREAD, &g_thread_ops);
    thread->tid = atomic_fetch_add_explicit(&g_next_tid, 1U, memory_order_relaxed);
    thread->process = process;
    object_get(process);
    thread->flags = THREAD_FLAG_INITIAL_PLACEMENT;
    atomic_init(&thread->state, THREAD_NEW);
    thread->kernel_stack_size = THREAD_KERNEL_STACK_SIZE;
    thread->kernel_stack_base = vmalloc(thread->kernel_stack_size);
    g_thread_create_stage = 2U;
    if (thread->kernel_stack_base == 0) {
        object_put(process);
        kfree(thread);
        return K_ENOMEM;
    }
    thread->kernel_stack_top = (vaddr_t)(uintptr_t)thread->kernel_stack_base +
                               thread->kernel_stack_size;
    thread->arch.fs_base = fs_base;
    thread->sched_class = SCHED_CLASS_FAIR;
    thread->base_sched_class = SCHED_CLASS_FAIR;
    thread->base_rt_priority = 0;
    thread->sched.weight = 1024U;
    thread->sched.nice = 0;
    list_init(&thread->sched.rt_node);
    list_init(&thread->process_node);
    list_init(&thread->global_node);
    list_init(&thread->owned_mutexes);
    thread->pi_blocked_on = 0;
    for (uint32_t i = 0; i < MAX_CPUS / 64U; ++i) thread->affinity.bits[i] = UINT64_MAX;

    uintptr_t frame_address = (thread->kernel_stack_top - sizeof(arch_trap_frame_t)) &
                              ~(uintptr_t)15U;
    arch_trap_frame_t *frame = (arch_trap_frame_t *)frame_address;
    uint8_t *frame_bytes = (uint8_t *)frame;
    for (size_t i = 0; i < sizeof(*frame); ++i) frame_bytes[i] = 0;
    frame->rip = entry;
    frame->cs = USER_CODE_SELECTOR;
    frame->rflags = USER_INITIAL_RFLAGS;
    frame->rsp = stack_top;
    frame->ss = USER_DATA_SELECTOR;
    frame->rdi = argument;

    uintptr_t switch_stack = frame_address - sizeof(uint64_t);
    *(uint64_t *)switch_stack = (uint64_t)(uintptr_t)&x86_user_thread_start;
    thread->arch.switch_ctx.rsp = switch_stack;
    thread->arch.switch_ctx.r12 = frame_address;
    thread->arch.switch_ctx.r13 = fs_base;
    thread->arch.switch_ctx.r14 = thread->kernel_stack_top;
    uint32_t current_cpu = x86_current_cpu_index();
    thread->current_cpu = (uint16_t)(current_cpu < MAX_CPUS ? current_cpu : 0U);
    if ((process->flags & PROCESS_FLAG_INIT_CPU_PINNED) != 0U &&
        current_cpu < MAX_CPUS) {
        for (uint32_t word = 0; word < MAX_CPUS / 64U; ++word) {
            thread->affinity.bits[word] = 0;
        }
        thread->affinity.bits[current_cpu >> 6] = 1ULL << (current_cpu & 63U);
    }

    process_lock(&process->thread_lock);
    if (atomic_load_explicit(&process->state, memory_order_acquire) != PROCESS_RUNNING) {
        process_unlock(&process->thread_lock);
        vfree(thread->kernel_stack_base);
        object_put(process);
        kfree(thread);
        return K_EBUSY;
    }
    list_insert_tail(&process->threads, &thread->process_node);
    ++process->thread_count;
    process->flags |= PROCESS_FLAG_EVER_HAD_THREAD;
    process_unlock(&process->thread_lock);
    /* 执行引用独立于用户句柄和创建者引用，直到线程死亡并离开其内核栈。 */
    object_get(thread);
    __atomic_fetch_or(&thread->flags, THREAD_FLAG_EXECUTION_REF, __ATOMIC_RELEASE);
    if (start) {
        atomic_store_explicit(&thread->state, THREAD_READY, memory_order_release);
        sched_enqueue(thread);
    }
    *out = thread;
    g_thread_create_stage = 3U;
    return K_OK;
}

kstatus_t thread_create_user(process_t *process, vaddr_t entry, vaddr_t stack_top,
                             vaddr_t fs_base, thread_t **out) {
    return thread_create_user_internal(process, entry, stack_top, fs_base, 0, true, out);
}

kstatus_t thread_create_user_suspended(process_t *process, vaddr_t entry,
                                       vaddr_t stack_top, vaddr_t fs_base,
                                       uint64_t argument, thread_t **out) {
    return thread_create_user_internal(process, entry, stack_top, fs_base,
                                       argument, false, out);
}

kstatus_t thread_start(thread_t *thread) {
    if (thread == 0) return K_EINVAL;
    unsigned expected = THREAD_NEW;
    if (!atomic_compare_exchange_strong_explicit(&thread->state, &expected, THREAD_READY,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
        return expected == THREAD_READY || expected == THREAD_RUNNING ? K_EBUSY : K_EINVAL;
    }
    sched_enqueue(thread);
    return K_OK;
}

void thread_release_execution_ref(thread_t *thread) {
    if (thread == 0) return;
    uint32_t old = __atomic_fetch_and(&thread->flags,
                                      ~THREAD_FLAG_EXECUTION_REF,
                                      __ATOMIC_ACQ_REL);
    if ((old & THREAD_FLAG_EXECUTION_REF) != 0) {
        process_t *process = thread->process;
        if (process != 0 &&
            atomic_load_explicit(&process->state, memory_order_acquire) == PROCESS_DEAD) {
            /* 当前线程已切离其内核栈和 CR3，此时才可销毁僵尸进程的地址空间。 */
            process_release_runtime_resources(process);
        }
        object_put(thread);
    }
}

uint32_t process_last_thread_create_stage(void) {
    return g_thread_create_stage;
}

kstatus_t thread_terminate(thread_t *thread, int64_t status) {
    if (thread == 0) return K_EINVAL;
    for (;;) {
        unsigned state = atomic_load_explicit(&thread->state, memory_order_acquire);
        if (state == THREAD_DEAD) return K_OK;
        if (state == THREAD_RUNNING) return K_EBUSY;
        if (state == THREAD_BLOCKED) {
            waiter_t *waiter = thread->blocked_waiter;
            if (waiter == 0 || !wait_cancel(waiter)) return K_EBUSY;
            continue;
        }
        thread->exit_code = status;
        if (atomic_compare_exchange_weak_explicit(&thread->state, &state, THREAD_DEAD,
                                                  memory_order_release,
                                                  memory_order_acquire)) break;
    }
    object_notify_signaled(thread);
    sched_remove(thread);
    thread_detach_from_process(thread);
    thread_release_execution_ref(thread);
    return K_OK;
}

static void thread_destroy(void *object) {
    thread_t *thread = (thread_t *)object;
    sched_remove(thread);
    process_t *process = thread->process;
    thread_detach_from_process(thread);
    vfree(thread->kernel_stack_base);
    if (process != 0) object_put(process);
    kfree(thread);
}

kstatus_t process_exec(process_t *process, const char __user *path,
                       const char __user *const __user *argv,
                       const char __user *const __user *envp) {
    (void)envp;
    return process_exec_from_vfs(process, path, argv);
}

void process_exit(int64_t status) {
    thread_t *thread = sched_current_thread();
    if (thread != 0 && thread->process != 0) {
        process_t *process = thread->process;
        process_lock(&process->thread_lock);
        if (atomic_load_explicit(&process->state, memory_order_acquire) == PROCESS_RUNNING) {
            process->exit_code = status;
            atomic_store_explicit(&process->state, PROCESS_EXITING, memory_order_release);
        }
        process_unlock(&process->thread_lock);

        /* 单核路径中其余线程均不在运行；逐个取得引用后终止，避免持锁调用调度代码。 */
        for (;;) {
            thread_t *target = 0;
            process_lock(&process->thread_lock);
            for (list_head_t *node = process->threads.next;
                 node != &process->threads; node = node->next) {
                thread_t *candidate = (thread_t *)((uint8_t *)node -
                    __builtin_offsetof(thread_t, process_node));
                if (candidate != thread &&
                    atomic_load_explicit(&candidate->state, memory_order_acquire) != THREAD_DEAD) {
                    object_get(candidate);
                    target = candidate;
                    break;
                }
            }
            process_unlock(&process->thread_lock);
            if (target == 0) break;
            kstatus_t terminate_status = thread_terminate(target, status);
            object_put(target);
            if (terminate_status != K_OK) break;
        }
    }
    thread_exit(status);
}

__noreturn void thread_exit(int64_t status) {
    thread_t *thread = sched_current_thread();
    if (thread != 0) {
        thread->exit_code = status;
        atomic_store_explicit(&thread->state, THREAD_DEAD, memory_order_release);
        object_notify_signaled(thread);
        thread_detach_from_process(thread);
    }
    schedule();
    /* 若当前 CPU 暂时没有可运行线程，仍须保持可中断，等待 IPI/定时器唤醒。 */
    for (;;) {
        __asm__ volatile ("sti; hlt" : : : "memory");
        schedule();
    }
}

static bool process_destruction_self_test_round(void) {
    process_t *parent = 0;
    process_t *child = 0;
    thread_t *thread = 0;
    handle_t child_handle = 0;
    void *lookup = 0;
    bool success = false;
    if (process_create(0, &parent) != K_OK ||
        thread_create_user(parent, 0x400000ULL, 0x800000ULL, 0, &thread) != K_OK ||
        parent->thread_count != 1U || thread->process != parent ||
        process_create(parent, &child) != K_OK || child->parent != parent ||
        handle_create(&parent->handles, child, 1U, &child_handle) != K_OK ||
        handle_lookup(&parent->handles, child_handle, 1U, &lookup) != K_OK ||
        lookup != child || handle_close(&parent->handles, child_handle) != K_OK ||
        handle_close(&parent->handles, child_handle) != K_ENOENT) goto cleanup;
    child_handle = 0;
    object_put(lookup);
    lookup = 0;
    if (thread_terminate(thread, K_ECANCELED) != K_OK) goto cleanup;
    uint32_t signaled_index = UINT32_MAX;
    int64_t wait_value = 0;
    void *wait_objects[2] = {thread, parent};
    if (parent->thread_count != 0U || !object_is_signaled(thread) ||
        !object_is_signaled(parent) || parent->exit_code != K_ECANCELED ||
        object_wait_many(wait_objects, 2U, true, 0, &signaled_index, &wait_value) != K_OK ||
        signaled_index != UINT32_MAX) goto cleanup;
    object_put(thread);
    thread = 0;
    success = true;

cleanup:
    if (lookup != 0) object_put(lookup);
    if (thread != 0) {
        (void)thread_terminate(thread, K_ECANCELED);
        object_put(thread);
    }
    if (child_handle != 0) (void)handle_close(&parent->handles, child_handle);
    if (child != 0) object_put(child);
    if (parent != 0) object_put(parent);
    return success;
}

bool process_core_self_test(void) {
    /* 多轮验证进程、线程、句柄的销毁以及引用计数不会累积泄漏。 */
    for (uint32_t round = 0; round < 32U; ++round) {
        if (!process_destruction_self_test_round()) return false;
    }
    return true;
}
