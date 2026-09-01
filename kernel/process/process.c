#include <arch/x86_64/context.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/paging.h>
#include <kernel/elf_loader.h>
#include <kernel/debug_stage.h>
#include <kernel/process.h>
#include <kernel/resource.h>
#include <kernel/sched.h>
#include <kernel/wait.h>
#include <kernel/window_server.h>
#include <uapi/process.h>
#include <uapi/syscall.h>
#include <uapi/wait.h>
#include "internal.h"

#define USER_CODE_SELECTOR    0x23ULL
#define USER_DATA_SELECTOR    0x1BULL
#define USER_INITIAL_RFLAGS   0x202ULL
#define THREAD_KERNEL_STACK_SIZE (64U * 1024U)

static atomic_uint_fast64_t g_next_task_id;
static atomic_uint g_process_init_state;
static spinlock_t g_process_registry_lock;
static list_head_t g_process_registry;
static spinlock_t g_process_teardown_lock;
static process_teardown_callback_t
    g_process_teardown_callbacks[PROCESS_TEARDOWN_CALLBACK_MAX];

extern void x86_user_thread_start(void);

static void process_destroy(void *object);
static bool process_is_signaled(const void *object);
static int64_t process_wait_value(const void *object);
static void process_global_initialize(void);
static void process_orphan_children(process_t *process);
static void process_detach_from_parent(process_t *process);

static void process_registry_lock(void) {
    process_global_initialize();
    sched_preempt_disable();
    spinlock_lock(&g_process_registry_lock);
}

static void process_registry_unlock(void) {
    spinlock_unlock(&g_process_registry_lock);
    sched_preempt_enable();
}

static void process_registry_remove(process_t *process) {
    bool removed = false;
    if (process == 0) return;
    process_registry_lock();
    if (process->global_registered) {
        list_del(&process->global_node);
        process->global_registered = false;
        removed = true;
    }
    process_registry_unlock();
    if (removed) object_put(process);
}

static void process_parent_lock(process_t *process) {
    sched_preempt_disable();
    spinlock_lock(&process->parent_lock);
}

static void process_parent_unlock(process_t *process) {
    spinlock_unlock(&process->parent_lock);
    sched_preempt_enable();
}

static void process_children_lock(process_t *process) {
    sched_preempt_disable();
    spinlock_lock(&process->children_lock);
}

static void process_children_unlock(process_t *process) {
    spinlock_unlock(&process->children_lock);
    sched_preempt_enable();
}

static void process_teardown_lock(void) {
    sched_preempt_disable();
    spinlock_lock(&g_process_teardown_lock);
}

static void process_teardown_unlock(void) {
    spinlock_unlock(&g_process_teardown_lock);
    sched_preempt_enable();
}

static uint32_t process_snapshot_teardown_callbacks(
    process_teardown_callback_t *callbacks) {
    uint32_t count = 0U;
    if (callbacks == 0) return 0U;
    process_teardown_lock();
    for (uint32_t index = 0U;
         index < PROCESS_TEARDOWN_CALLBACK_MAX; ++index) {
        process_teardown_callback_t callback =
            g_process_teardown_callbacks[index];
        if (callback != 0) callbacks[count++] = callback;
    }
    process_teardown_unlock();
    return count;
}

static const object_ops_t g_process_ops = {
    .destroy = process_destroy,
    .type_name = "Process",
    .is_signaled = process_is_signaled,
    .wait_value = process_wait_value,
};

static process_t *process_parent_reference(process_t *process) {
    process_t *parent;
    if (process == 0) return 0;
    process_parent_lock(process);
    parent = process->parent;
    if (parent != 0) object_get(parent);
    process_parent_unlock(process);
    return parent;
}

static void process_notify_parent(process_t *process) {
    process_t *parent = process_parent_reference(process);
    if (parent == 0) return;
    (void)wake_all(&parent->child_waitq);
    object_put(parent);
}

static void process_orphan_children(process_t *process) {
    if (process == 0) return;
    for (;;) {
        process_t *child;
        bool related;

        process_children_lock(process);
        if (list_empty(&process->children)) {
            process_children_unlock(process);
            return;
        }
        child = list_entry(process->children.next, process_t, child_node);
        list_del(&child->child_node);
        process_parent_lock(child);
        related = child->parent == process;
        if (related) child->parent = 0;
        process_parent_unlock(child);
        process_children_unlock(process);

        /* The list owns one child reference and the relation owns one
         * parent reference.  Neither is released while the list lock is held. */
        object_put(child);
        if (related) object_put(process);
    }
}

static void process_detach_from_parent(process_t *process) {
    process_t *parent;
    bool related = false;
    bool detached = false;

    if (process == 0) return;
    parent = process_parent_reference(process);
    if (parent == 0) return;

    /* Re-check the relation after taking the parent list lock.  This avoids
     * taking the inverse lock order used by process_orphan_children(). */
    process_children_lock(parent);
    process_parent_lock(process);
    if (process->parent == parent) {
        related = true;
        if (process->child_node.next != &process->child_node) {
            list_del(&process->child_node);
            detached = true;
        }
        process->parent = 0;
    }
    process_parent_unlock(process);
    process_children_unlock(parent);

    if (detached) object_put(process); /* parent child-list reference */
    if (related) object_put(parent);   /* child parent-relation reference */
    object_put(parent);                /* temporary lookup reference */
}

static bool process_child_matches(const process_t *child, int64_t pid) {
    if (child == 0) return false;
    return pid == -1 || (pid > 0 && child->pid == (uint64_t)pid);
}

typedef struct process_wait_context {
    process_t *parent;
    int64_t pid;
    bool has_child;
    process_t *candidate;
} process_wait_context_t;

static bool process_wait_predicate(void *opaque) {
    process_wait_context_t *context = (process_wait_context_t *)opaque;
    context->has_child = false;
    context->candidate = 0;
    process_children_lock(context->parent);
    for (list_head_t *node = context->parent->children.next;
         node != &context->parent->children; node = node->next) {
        process_t *child = list_entry(node, process_t, child_node);
        if (!process_child_matches(child, context->pid)) continue;
        context->has_child = true;
        if (atomic_load_explicit(&child->state, memory_order_acquire) ==
            PROCESS_DEAD) {
            object_get(child);
            context->candidate = child;
            break;
        }
    }
    process_children_unlock(context->parent);
    /* A disappearing child is also a terminal result for this wait. */
    return !context->has_child || context->candidate != 0;
}

void process_detach_thread(thread_t *thread) {
    process_t *process = thread != 0 ? thread->process : 0;
    if (process == 0) return;
    bool notify_process = false;
    process_thread_lock(process);
    if (thread->process_node.next != &thread->process_node) {
        list_del(&thread->process_node);
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
    process_thread_unlock(process);
    if (notify_process) process_registry_remove(process);
    if (notify_process) {
        process_orphan_children(process);
        process_teardown_callback_t callbacks[PROCESS_TEARDOWN_CALLBACK_MAX];
        uint32_t callback_count =
            process_snapshot_teardown_callbacks(callbacks);
        for (uint32_t index = 0U; index < callback_count; ++index) {
            callbacks[index](process);
        }
    }
    if (notify_process) {
        object_notify_signaled(process);
        process_notify_parent(process);
    }
}

static bool process_is_signaled(const void *object) {
    const process_t *process = (const process_t *)object;
    return atomic_load_explicit(&process->state, memory_order_acquire) == PROCESS_DEAD;
}

static int64_t process_wait_value(const void *object) {
    return ((const process_t *)object)->exit_code;
}

void process_set_name(process_t *process, const char *path) {
    const char *base;
    size_t length = 0U;
    if (process == 0 || path == 0 || *path == '\0') return;
    base = path;
    for (const char *cursor = path; *cursor != '\0'; ++cursor) {
        if (*cursor == '/') base = cursor + 1U;
    }
    while (base[length] != '\0' && length + 1U < OS_PROCESS_NAME_MAX) ++length;
    if (length == 0U) return;
    sched_preempt_disable();
    spinlock_lock(&process->info_lock);
    for (size_t index = 0U; index < OS_PROCESS_NAME_MAX; ++index) {
        process->name[index] = index < length ? base[index] : '\0';
    }
    process->name[length] = '\0';
    spinlock_unlock(&process->info_lock);
    sched_preempt_enable();
}

static void process_copy_name(process_t *destination,
                              process_t *source) {
    char name[OS_PROCESS_NAME_MAX];
    if (destination == 0 || source == 0) return;
    sched_preempt_disable();
    spinlock_lock(&source->info_lock);
    for (size_t index = 0U; index < sizeof(name); ++index) {
        name[index] = source->name[index];
    }
    spinlock_unlock(&source->info_lock);
    sched_preempt_enable();
    process_set_name(destination, name);
}

kstatus_t process_enumerate(uint32_t index, os_process_snapshot_t *snapshot) {
    uint32_t current = 0U;
    if (snapshot == 0) return K_EINVAL;
    process_registry_lock();
    for (list_head_t *node = g_process_registry.next;
         node != &g_process_registry; node = node->next) {
        process_t *process = list_entry(node, process_t, global_node);
        if (current++ != index) continue;
        snapshot->hdr.size = sizeof(*snapshot);
        snapshot->hdr.version = OS_SYSCALL_ABI_VERSION;
        snapshot->hdr.flags = 0U;
        snapshot->pid = process->pid;
        process_parent_lock(process);
        snapshot->parent_pid = process->parent != 0 ? process->parent->pid : 0U;
        process_parent_unlock(process);
        process_thread_lock(process);
        snapshot->state = atomic_load_explicit(&process->state, memory_order_acquire);
        snapshot->thread_count = process->thread_count;
        snapshot->exit_code = process->exit_code;
        process_thread_unlock(process);
        snapshot->create_time_ns = process->create_time_ns;
        sched_preempt_disable();
        spinlock_lock(&process->info_lock);
        for (uint32_t name_index = 0U; name_index < OS_PROCESS_NAME_MAX;
             ++name_index) {
            snapshot->name[name_index] = process->name[name_index];
        }
        spinlock_unlock(&process->info_lock);
        sched_preempt_enable();
        process_registry_unlock();
        return K_OK;
    }
    process_registry_unlock();
    return K_ENOENT;
}

kstatus_t process_enumerate_thread(uint32_t index,
                                   os_thread_snapshot_t *snapshot) {
    uint32_t current = 0U;
    if (snapshot == 0) return K_EINVAL;
    process_registry_lock();
    for (list_head_t *process_node = g_process_registry.next;
         process_node != &g_process_registry; process_node = process_node->next) {
        process_t *process = list_entry(process_node, process_t, global_node);
        process_thread_lock(process);
        for (list_head_t *thread_node = process->threads.next;
             thread_node != &process->threads; thread_node = thread_node->next) {
            if (current++ != index) continue;
            thread_t *thread = list_entry(thread_node, thread_t, process_node);
            snapshot->hdr.size = sizeof(*snapshot);
            snapshot->hdr.version = OS_SYSCALL_ABI_VERSION;
            snapshot->hdr.flags = 0U;
            snapshot->tid = thread->tid;
            snapshot->process_pid = process->pid;
            snapshot->state = atomic_load_explicit(&thread->state,
                                                   memory_order_acquire);
            snapshot->current_cpu = thread->current_cpu;
            sched_preempt_disable();
            spinlock_lock(&process->info_lock);
            for (uint32_t name_index = 0U; name_index < OS_THREAD_NAME_MAX;
                 ++name_index) {
                snapshot->name[name_index] = name_index < OS_PROCESS_NAME_MAX ?
                    process->name[name_index] : '\0';
            }
            spinlock_unlock(&process->info_lock);
            sched_preempt_enable();
            process_thread_unlock(process);
            process_registry_unlock();
            return K_OK;
        }
        process_thread_unlock(process);
    }
    process_registry_unlock();
    return K_ENOENT;
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
        atomic_init(&g_next_task_id, 1U);
        spinlock_init(&g_process_registry_lock);
        list_init(&g_process_registry);
        spinlock_init(&g_process_teardown_lock);
        atomic_store_explicit(&g_process_init_state, 2U, memory_order_release);
        return;
    }
    while (atomic_load_explicit(&g_process_init_state, memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
}

uint64_t process_allocate_task_id(void) {
    process_global_initialize();
    return atomic_fetch_add_explicit(&g_next_task_id, 1U,
                                     memory_order_relaxed);
}

kstatus_t process_register_teardown_callback(
    process_teardown_callback_t callback) {
    if (callback == 0) return K_EINVAL;
    process_global_initialize();
    process_teardown_lock();
    for (uint32_t index = 0U;
         index < PROCESS_TEARDOWN_CALLBACK_MAX; ++index) {
        if (g_process_teardown_callbacks[index] == callback) {
            process_teardown_unlock();
            return K_OK;
        }
    }
    for (uint32_t index = 0U;
         index < PROCESS_TEARDOWN_CALLBACK_MAX; ++index) {
        if (g_process_teardown_callbacks[index] == 0) {
            g_process_teardown_callbacks[index] = callback;
            process_teardown_unlock();
            return K_OK;
        }
    }
    process_teardown_unlock();
    return K_ENOSPC;
}

void process_initialize_object(object_header_t *header, object_type_id_t type,
                               const object_ops_t *ops) {
    refcount_init(&header->refs, 1U);
    header->type = type;
    header->flags = 0;
    header->ops = ops;
}

kstatus_t process_create_named(process_t *parent, const char *name,
                               process_t **out) {
    if (out == 0) return K_EINVAL;
    process_global_initialize();
    process_t *process = (process_t *)kzalloc(sizeof(process_t), 0);
    if (process == 0) return K_ENOMEM;
    process_initialize_object(&process->object, KOBJECT_TYPE_PROCESS,
                              &g_process_ops);
    process->pid = process_allocate_task_id();
    atomic_init(&process->state, PROCESS_NEW);
    if (parent != 0) {
        process->flags = parent->flags & PROCESS_FLAG_INIT_CPU_PINNED;
    }
    atomic_init(&process->parent_lock.state, 0U);
    atomic_init(&process->children_lock.state, 0U);
    wait_queue_init(&process->child_waitq);
    list_init(&process->children);
    list_init(&process->child_node);
    atomic_init(&process->thread_lock.state, 0U);
    atomic_init(&process->signal_lock.state, 0U);
    for (uint32_t signal = 0U; signal <= OS_SIGNAL_COUNT; ++signal) {
        process->signal_actions[signal].handler = OS_SIG_DFL;
    }
    list_init(&process->threads);
    list_init(&process->job_node);
    list_init(&process->global_node);
    spinlock_init(&process->info_lock);
    process->global_registered = false;
    if (name != 0 && *name != '\0') process_set_name(process, name);
    else if (parent != 0) process_copy_name(process, parent);
    else process_set_name(process, "system");
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
    process->parent = 0;
    if (parent != 0) {
        process_parent_lock(process);
        process->parent = parent;
        object_get(parent);
        process_parent_unlock(process);
        status = parent->job != 0 ? job_attach_process(parent->job, process) : K_OK;
        if (status != K_OK) {
            process_detach_from_parent(process);
            handle_table_destroy(&process->handles);
            vm_space_put(process->vm);
            kfree(process);
            return status;
        }
        process_children_lock(parent);
        if (atomic_load_explicit(&parent->state, memory_order_acquire) !=
            PROCESS_RUNNING) {
            process_children_unlock(parent);
            (void)job_detach_process(process);
            process_detach_from_parent(process);
            handle_table_destroy(&process->handles);
            vm_space_put(process->vm);
            kfree(process);
            return K_EBUSY;
        }
        object_get(process);
        list_add_tail(&parent->children, &process->child_node);
        process_children_unlock(parent);
    } else {
        job_t *root_job = 0;
        status = job_create(0, 0, &root_job);
        if (status == K_OK) status = job_attach_process(root_job, process);
        if (root_job != 0) object_put(root_job);
        if (status != K_OK) {
            handle_table_destroy(&process->handles);
            vm_space_put(process->vm);
            kfree(process);
            return status;
        }
    }
    atomic_store_explicit(&process->state, PROCESS_RUNNING, memory_order_release);
    process_registry_lock();
    object_get(process);
    list_add_tail(&g_process_registry, &process->global_node);
    process->global_registered = true;
    process_registry_unlock();
    *out = process;
    return K_OK;
}

kstatus_t process_create(process_t *parent, process_t **out) {
    return process_create_named(parent, 0, out);
}

static void process_destroy(void *object) {
    process_t *process = (process_t *)object;
    process_registry_remove(process);
    atomic_store_explicit(&process->state, PROCESS_DEAD, memory_order_release);
    process_orphan_children(process);
    process_detach_from_parent(process);
    (void)job_detach_process(process);
    process_release_runtime_resources(process);
    kfree(process);
}

kstatus_t process_abort(process_t *process) {
    if (process == 0) return K_EINVAL;
    atomic_store_explicit(&process->state, PROCESS_DEAD, memory_order_release);
    process_registry_remove(process);
    process_orphan_children(process);
    process_detach_from_parent(process);
    (void)job_detach_process(process);
    process_release_runtime_resources(process);
    return K_OK;
}

kstatus_t process_fork(process_t *parent, process_t **out) {
    if (parent == 0 || out == 0) return K_EINVAL;
    process_t *child = 0;
    kstatus_t status = process_create(parent, &child);
    if (status != K_OK) return status;

    handle_table_t cloned = {0};
    /* Windows are owner-bound resources.  A forked command must not inherit
     * the shell's window handle and close the parent's window on exit. */
    status = handle_table_clone_without_type(&parent->handles, &cloned,
                                             KOBJECT_TYPE_WINDOW);
    if (status != K_OK) {
        (void)process_abort(child);
        object_put(child);
        return status;
    }
    handle_table_destroy(&child->handles);
    child->handles = cloned;
    spinlock_lock(&parent->signal_lock);
    spinlock_lock(&child->signal_lock);
    for (uint32_t signal = 0U; signal <= OS_SIGNAL_COUNT; ++signal) {
        child->signal_actions[signal] = parent->signal_actions[signal];
    }
    spinlock_unlock(&child->signal_lock);
    spinlock_unlock(&parent->signal_lock);
    *out = child;
    return K_OK;
}

uint64_t process_parent_pid(const process_t *process) {
    uint64_t pid = 0;
    if (process == 0) return 0;
    process_parent_lock((process_t *)process);
    if (process->parent != 0) pid = process->parent->pid;
    process_parent_unlock((process_t *)process);
    return pid;
}

kstatus_t process_wait_child(process_t *parent, int64_t pid, uint32_t options,
                             int64_t *child_pid, int64_t *exit_code,
                             uint32_t *exit_signal) {
    if (parent == 0 || child_pid == 0 || exit_code == 0 || exit_signal == 0 ||
        (options & ~OS_PROCESS_WAIT_NOHANG) != 0U ||
        (pid != -1 && pid <= 0)) return K_EINVAL;

    process_wait_context_t context = {
        .parent = parent,
        .pid = pid,
        .has_child = false,
        .candidate = 0,
    };
    uint64_t timeout = (options & OS_PROCESS_WAIT_NOHANG) != 0U ? 0U :
                       OS_WAIT_INFINITE;
    for (;;) {
        kstatus_t status = wait_on_queue(&parent->child_waitq,
                                          process_wait_predicate, &context,
                                          timeout);
        if (status == K_ETIMEDOUT) {
            if (!context.has_child) return K_ECHILD;
            *child_pid = 0;
            *exit_code = 0;
            *exit_signal = 0U;
            return K_OK;
        }
        if (status != K_OK) return status;
        if (!context.has_child) return K_ECHILD;

        process_t *child = context.candidate;
        bool reaped = false;
        int64_t status_value = 0;
        uint32_t signal_value = 0U;
        uint64_t result_pid = 0;
        process_children_lock(parent);
        process_parent_lock(child);
        if (child->parent == parent &&
            child->child_node.next != &child->child_node &&
            atomic_load_explicit(&child->state, memory_order_acquire) ==
                PROCESS_DEAD) {
            list_del(&child->child_node);
            child->parent = 0;
            result_pid = child->pid;
            status_value = child->exit_code;
            signal_value = child->exit_signal;
            reaped = true;
        }
        process_parent_unlock(child);
        process_children_unlock(parent);
        if (!reaped) {
            object_put(child);
            timeout = (options & OS_PROCESS_WAIT_NOHANG) != 0U ? 0U :
                      OS_WAIT_INFINITE;
            continue;
        }
        object_put(parent); /* child parent-relation reference */
        object_put(child);  /* child-list reference */
        object_put(child);  /* predicate reference */
        (void)wake_all(&parent->child_waitq);
        *child_pid = (int64_t)result_pid;
        *exit_code = status_value;
        *exit_signal = signal_value;
        return K_OK;
    }
}

void process_release_runtime_resources(process_t *process) {
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
         parent->pid == child->pid || parent->pid == thread->tid ||
         child->pid == thread->tid ||
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
    if (child != 0) {
        (void)process_abort(child);
        object_put(child);
    }
    if (parent != 0) {
        if (thread == 0) (void)process_abort(parent);
        object_put(parent);
    }
    return success;
}

bool process_core_self_test(void) {
    /* 多轮验证进程、线程、句柄的销毁以及引用计数不会累积泄漏。 */
    for (uint32_t round = 0; round < 32U; ++round) {
        if (!process_destruction_self_test_round()) return false;
    }
    return true;
}
