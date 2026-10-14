#include <arch/x86_64/context.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/smp.h>
#include <kernel/console.h>
#include <kernel/elf_loader.h>
#include <kernel/kmem.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/vfs.h>
#include "internal.h"

#ifdef OS_KERNEL_BUILD

/* REFACTOR_P7A_SHELL_LAUNCHER_OWNER: desktop app launch and title policy. */

#define DESKTOP_PROGRAM_MAX_BYTES (16ULL * 1024ULL * 1024ULL)

#define DESKTOP_LAUNCH_WORKER_STACK_SIZE (32U * 1024U)
#define DESKTOP_CHROME_LAUNCH_STARTING UINT64_MAX

static uint8_t g_desktop_launch_worker_stack
    [DESKTOP_LAUNCH_WORKER_STACK_SIZE]
    __attribute__((aligned(16)));

static bool desktop_chrome_launch_claim(
    atomic_uint_fast64_t *launch_pid) {
    uint_fast64_t expected = 0U;
    return atomic_compare_exchange_strong_explicit(
        launch_pid, &expected, DESKTOP_CHROME_LAUNCH_STARTING,
        memory_order_acq_rel, memory_order_acquire);
}

static bool desktop_chrome_launch_publish_pid(
    atomic_uint_fast64_t *launch_pid, uint64_t pid) {
    uint_fast64_t expected = DESKTOP_CHROME_LAUNCH_STARTING;
    return pid != 0U && pid != DESKTOP_CHROME_LAUNCH_STARTING &&
           atomic_compare_exchange_strong_explicit(
               launch_pid, &expected, pid,
               memory_order_acq_rel, memory_order_acquire);
}

static bool desktop_chrome_launch_release(
    atomic_uint_fast64_t *launch_pid, uint64_t pid) {
    uint_fast64_t expected = pid;
    return pid != 0U && atomic_compare_exchange_strong_explicit(
        launch_pid, &expected, 0U,
        memory_order_acq_rel, memory_order_acquire);
}

/* Window lifetime and launch requests share WindowLock. Startup double clicks
 * coalesce, but a click after the last window closes survives process teardown. */
static bool desktop_chrome_request_launch(void) {
    bool claimed;
    window_lock();
    claimed = desktop_chrome_launch_claim(
        &g_window_server.desktop_chrome_launch_pid);
    if (claimed) {
        g_window_server.desktop_chrome_closing_pid = 0U;
        atomic_store_explicit(&g_window_server.desktop_chrome_relaunch_pending,
                              false, memory_order_release);
    } else {
        uint64_t pid = atomic_load_explicit(
            &g_window_server.desktop_chrome_launch_pid, memory_order_acquire);
        if (pid != 0U && pid != DESKTOP_CHROME_LAUNCH_STARTING &&
            pid == g_window_server.desktop_chrome_closing_pid) {
            atomic_store_explicit(
                &g_window_server.desktop_chrome_relaunch_pending,
                true, memory_order_release);
        }
    }
    window_unlock();
    return claimed;
}

void desktop_chrome_window_opened_locked(process_t *owner) {
    if (owner == 0 || owner->pid != atomic_load_explicit(
            &g_window_server.desktop_chrome_launch_pid, memory_order_acquire)) {
        return;
    }
    g_window_server.desktop_chrome_closing_pid = 0U;
    atomic_store_explicit(&g_window_server.desktop_chrome_relaunch_pending,
                          false, memory_order_release);
}

void desktop_chrome_window_closed_locked(process_t *owner) {
    if (owner == 0 || owner->pid != atomic_load_explicit(
            &g_window_server.desktop_chrome_launch_pid, memory_order_acquire)) {
        return;
    }
    for (uint32_t i = 0U; i < g_window_server.count; ++i) {
        if (g_window_server.windows[i] != 0 &&
            g_window_server.windows[i]->owner == owner) return;
    }
    g_window_server.desktop_chrome_closing_pid = owner->pid;
}

bool desktop_chrome_process_released_locked(uint64_t pid) {
    if (!desktop_chrome_launch_release(
            &g_window_server.desktop_chrome_launch_pid, pid)) return false;
    g_window_server.desktop_chrome_closing_pid = 0U;
    return atomic_load_explicit(&g_window_server.desktop_chrome_relaunch_pending,
                                memory_order_acquire);
}

bool desktop_chrome_launch_guard_self_test(void) {
    atomic_uint_fast64_t launch_pid;
    atomic_init(&launch_pid, 0U);

    if (!desktop_chrome_launch_claim(&launch_pid) ||
        desktop_chrome_launch_claim(&launch_pid) ||
        !desktop_chrome_launch_publish_pid(&launch_pid, 42U) ||
        desktop_chrome_launch_release(&launch_pid, 41U) ||
        atomic_load_explicit(&launch_pid, memory_order_acquire) != 42U ||
        desktop_chrome_launch_claim(&launch_pid) ||
        !desktop_chrome_launch_release(&launch_pid, 42U) ||
        atomic_load_explicit(&launch_pid, memory_order_acquire) != 0U ||
        !desktop_chrome_launch_claim(&launch_pid) ||
        !desktop_chrome_launch_release(
            &launch_pid, DESKTOP_CHROME_LAUNCH_STARTING) ||
        !desktop_chrome_launch_claim(&launch_pid) ||
        !desktop_chrome_launch_publish_pid(&launch_pid, 43U) ||
        !desktop_chrome_launch_release(&launch_pid, 43U)) {
        return false;
    }
    return true;
}

const char *window_shell_program_path(uint32_t app) {
    if (app == DESKTOP_APP_FILES) return "/sbin/fileman";
    if (app == DESKTOP_APP_TERMINAL) return "/sbin/gshell";
    if (app == DESKTOP_APP_NOTES) return "/sbin/notepad";
    if (app == DESKTOP_APP_NETWORK) return "/sbin/netmgr";
    if (app == DESKTOP_APP_TASKMGR) return "/sbin/taskmgr";
    if (app == DESKTOP_APP_CHROME) return "/sbin/chrome-start";
    return 0;
}

const char *window_shell_app_title(uint32_t app) {
    if (app == DESKTOP_APP_FILES) return "FILEMAN";
    if (app == DESKTOP_APP_TERMINAL) return "SHELL";
    if (app == DESKTOP_APP_NOTES) return "NOTEPAD";
    if (app == DESKTOP_APP_NETWORK) return "NETWORK";
    if (app == DESKTOP_APP_TASKMGR) return "TASK MANAGER";
    if (app == DESKTOP_APP_CHROME) return "CHROME";
    return 0;
}

bool window_shell_title_matches(const char *title, const char *expected) {
    uint32_t index = 0U;
    if (title == 0 || expected == 0) return false;
    while (index < 32U) {
        if (title[index] != expected[index]) return false;
        if (expected[index] == '\0') return true;
        ++index;
    }
    return false;
}

static bool desktop_launch_worker_ready(void *context) {
    (void)context;
    return atomic_load_explicit(&g_window_server.desktop_launch_queue_count,
                                memory_order_acquire) != 0U ||
           (atomic_load_explicit(&g_window_server.desktop_chrome_relaunch_pending,
                                 memory_order_acquire) &&
            atomic_load_explicit(&g_window_server.desktop_chrome_launch_pid,
                                 memory_order_acquire) == 0U);
}

static bool desktop_launch_dequeue(uint32_t *app) {
    uint32_t count;

    if (app == 0) return false;

    window_lock();
    count = atomic_load_explicit(&g_window_server.desktop_launch_queue_count,
                                 memory_order_relaxed);
    if (count == 0U) {
        if (atomic_load_explicit(
                &g_window_server.desktop_chrome_relaunch_pending,
                memory_order_relaxed) &&
            atomic_load_explicit(&g_window_server.desktop_chrome_launch_pid,
                                 memory_order_acquire) == 0U) {
            atomic_store_explicit(
                &g_window_server.desktop_chrome_relaunch_pending,
                false, memory_order_release);
            *app = DESKTOP_APP_CHROME;
            window_unlock();
            return true;
        }
        window_unlock();
        return false;
    }

    *app = g_window_server.desktop_launch_queue[
        g_window_server.desktop_launch_queue_head];
    g_window_server.desktop_launch_queue_head =
        (g_window_server.desktop_launch_queue_head + 1U) %
        WINDOW_DESKTOP_LAUNCH_QUEUE_CAPACITY;
    atomic_store_explicit(&g_window_server.desktop_launch_queue_count,
                          count - 1U, memory_order_release);
    window_unlock();
    return true;
}

bool desktop_launch_enqueue(uint32_t app) {
    uint32_t count;

    if (window_shell_program_path(app) == 0 ||
        !atomic_load_explicit(&g_window_server.desktop_launch_worker_started,
                              memory_order_acquire)) {
        return false;
    }

    window_lock();
    count = atomic_load_explicit(&g_window_server.desktop_launch_queue_count,
                                 memory_order_relaxed);
    if (count >= WINDOW_DESKTOP_LAUNCH_QUEUE_CAPACITY) {
        window_unlock();
        return false;
    }

    g_window_server.desktop_launch_queue[
        g_window_server.desktop_launch_queue_tail] = app;
    g_window_server.desktop_launch_queue_tail =
        (g_window_server.desktop_launch_queue_tail + 1U) %
        WINDOW_DESKTOP_LAUNCH_QUEUE_CAPACITY;
    atomic_store_explicit(&g_window_server.desktop_launch_queue_count,
                          count + 1U, memory_order_release);
    window_unlock();

    (void)wake_one(&g_window_server.desktop_launch_waitq);
    return true;
}

static void __attribute__((noreturn)) desktop_launch_worker_main(
    void *argument) {
    uint32_t app;
    (void)argument;

    for (;;) {
        if (wait_on_queue(&g_window_server.desktop_launch_waitq,
                          desktop_launch_worker_ready, 0,
                          UINT64_MAX) != K_OK) {
            continue;
        }

        while (desktop_launch_dequeue(&app)) {
            if (desktop_restore_minimized_app(app)) continue;

            kstatus_t status = desktop_launch_program(app);
            (void)status;
        }
    }
}

bool desktop_shell_start_launch_worker(uint32_t compositor_cpu) {
    uint32_t current_cpu = x86_current_cpu_index();
    uint32_t launch_cpu = UINT32_MAX;
    thread_t *worker;

    if (atomic_load_explicit(&g_window_server.desktop_launch_worker_started,
                             memory_order_acquire)) {
        return true;
    }

    /* Keep launch I/O away from both the bootstrap CPU and compositor when a
     * spare AP exists.  On a two-CPU guest, sharing the bootstrap CPU still
     * leaves the compositor free to consume pointer input. */
    for (uint32_t candidate = 0U;
         candidate < x86_smp_discovered_count() && candidate < MAX_CPUS;
         ++candidate) {
        if (candidate == current_cpu || candidate == compositor_cpu ||
            !x86_smp_cpu_online(candidate)) {
            continue;
        }
        launch_cpu = candidate;
        break;
    }
    if (launch_cpu == UINT32_MAX) launch_cpu = current_cpu;
    if (launch_cpu >= MAX_CPUS) return false;

    worker = &g_window_server.desktop_launch_worker;
    memset(worker, 0, sizeof(*worker));

    refcount_init(&worker->object.refs, 1U);
    worker->object.type = KOBJECT_TYPE_THREAD;
    worker->object.flags = 0U;
    worker->object.ops = 0;
    worker->tid = UINT64_MAX - 5ULL;
    worker->process = 0;
    atomic_init(&worker->state, THREAD_READY);
    atomic_init(&worker->block_epoch, 0U);
    atomic_init(&worker->command_ack, 0U);
    worker->kernel_stack_base = g_desktop_launch_worker_stack;
    worker->kernel_stack_size = sizeof(g_desktop_launch_worker_stack);
    worker->kernel_stack_top =
        ((vaddr_t)(uintptr_t)g_desktop_launch_worker_stack +
         sizeof(g_desktop_launch_worker_stack)) & ~(vaddr_t)0x0FULL;
    worker->sched_class = SCHED_CLASS_FAIR;
    worker->base_sched_class = SCHED_CLASS_FAIR;
    worker->rt_priority = 0U;
    worker->base_rt_priority = 0U;
    worker->sched.weight = 1024U;
    worker->sched.nice = 0;
    worker->sched.vruntime = 0U;
    list_init(&worker->sched.rt_node);
    list_init(&worker->process_node);
    list_init(&worker->owned_mutexes);
    for (uint32_t word = 0U; word < MAX_CPUS / 64U; ++word) {
        worker->affinity.bits[word] = 0U;
    }
    worker->affinity.bits[launch_cpu >> 6] =
        1ULL << (launch_cpu & 63U);
    worker->owner_cpu = (uint16_t)launch_cpu;
    worker->current_cpu = (uint16_t)launch_cpu;

    uintptr_t stack_top = (uintptr_t)worker->kernel_stack_top;
    uintptr_t switch_stack = stack_top - sizeof(uint64_t);
    *(uint64_t *)switch_stack =
        (uint64_t)(uintptr_t)&x86_kernel_thread_start;
    worker->arch.switch_ctx.rsp = switch_stack;
    worker->arch.switch_ctx.r12 =
        (uint64_t)(uintptr_t)&desktop_launch_worker_main;
    worker->arch.switch_ctx.r13 = 0U;
    worker->arch.switch_ctx.r14 = stack_top;
    worker->arch.fs_base = 0U;

    atomic_store_explicit(&g_window_server.desktop_launch_worker_started,
                          true, memory_order_release);
    if (!sched_enqueue_bootstrap(worker)) {
        atomic_store_explicit(&g_window_server.desktop_launch_worker_started,
                              false, memory_order_release);
        return false;
    }
    return true;
}

static bool desktop_window_matches_app_locked(
    const window_server_window_t *window, uint32_t app,
    const char *expected_title) {
    if (app != DESKTOP_APP_CHROME)
        return window_shell_title_matches(window->title, expected_title);
    if (window->owner == 0) return false;

    /* Chrome's native window currently has no fixed kernel title; page titles
     * are not a stable identifier.  Match its owning process instead. */
    process_t *owner = window->owner;
    spinlock_lock(&owner->info_lock);
    bool matches = window_shell_title_matches(owner->name, "chrome");
    spinlock_unlock(&owner->info_lock);
    return matches;
}

bool desktop_restore_minimized_app(uint32_t app) {
    const char *expected = window_shell_app_title(app);
    window_server_window_t *window = 0;
    if (expected == 0) return false;

    window_lock();
    for (uint32_t index = g_window_server.count; index != 0U; --index) {
        window_server_window_t *candidate =
            g_window_server.windows[index - 1U];
        if (candidate == 0 ||
            (!candidate->minimized && app != DESKTOP_APP_CHROME) ||
            (candidate->flags & OS_WINDOW_VISIBLE) == 0U ||
            !desktop_window_matches_app_locked(candidate, app, expected)) {
            continue;
        }
        window = candidate;
        break;
    }
    if (window == 0) {
        window_unlock();
        return false;
    }

    bool was_minimized = window->minimized;
    window->minimized = false;
    window->dirty = true;
    window_scene_focus_locked(window);
    window_mark_window_locked(window);
    if (was_minimized) window_enqueue_minimized_event_locked(window);
    window_unlock();
    (void)wake_all(&g_window_server.event_waitq);
    return true;
}

kstatus_t desktop_launch_program(uint32_t app) {
    const char *path = window_shell_program_path(app);
    bool chrome_launch_claimed = app == DESKTOP_APP_CHROME;
    uint64_t chrome_launch_token = 0U;
    file_t *file = 0;
    process_t *process = 0;
    thread_t *thread = 0;
    user_elf_image_info_t info;
    vaddr_t stack_pointer = 0U;
    uint8_t *image = 0;
    uint64_t image_size;
    uint64_t bytes = 0U;
    kstatus_t status;

    if (path == 0) return K_EINVAL;
    if (chrome_launch_claimed) {
        if (!desktop_chrome_request_launch()) {
            return K_OK;
        }
        chrome_launch_token = DESKTOP_CHROME_LAUNCH_STARTING;
    }
    status = vfs_open_kernel(path, VFS_OPEN_READ, 0U, &file);
    if (status != K_OK) {
        if (chrome_launch_claimed) {
            (void)desktop_chrome_launch_release(
                &g_window_server.desktop_chrome_launch_pid,
                chrome_launch_token);
        }
        return status;
    }
    if (file->vnode == 0 || file->vnode->size == 0U ||
        file->vnode->size > DESKTOP_PROGRAM_MAX_BYTES) {
        vfs_close(file);
        if (chrome_launch_claimed) {
            (void)desktop_chrome_launch_release(
                &g_window_server.desktop_chrome_launch_pid,
                chrome_launch_token);
        }
        return K_EINVAL;
    }

    image_size = file->vnode->size;
    image = (uint8_t *)kmalloc((size_t)image_size, 0);
    if (image == 0) {
        vfs_close(file);
        if (chrome_launch_claimed) {
            (void)desktop_chrome_launch_release(
                &g_window_server.desktop_chrome_launch_pid,
                chrome_launch_token);
        }
        return K_ENOMEM;
    }
    status = vfs_read_kernel(file, image, image_size, &bytes);
    vfs_close(file);
    if (status != K_OK || bytes != image_size) {
        kfree(image);
        if (chrome_launch_claimed) {
            (void)desktop_chrome_launch_release(
                &g_window_server.desktop_chrome_launch_pid,
                chrome_launch_token);
        }
        return status == K_OK ? K_EIO : status;
    }

    status = process_create(0, &process);
    if (status == K_OK) {
        if (chrome_launch_claimed) {
            if (!desktop_chrome_launch_publish_pid(
                    &g_window_server.desktop_chrome_launch_pid,
                    process->pid)) {
                (void)process_abort(process);
                object_put(process);
                (void)desktop_chrome_launch_release(
                    &g_window_server.desktop_chrome_launch_pid,
                    chrome_launch_token);
                return K_EBUSY;
            }
            chrome_launch_token = process->pid;
        }
        process_set_name(process, path);
        status = process_load_elf_image(process, image, (size_t)image_size,
                                        &info, &stack_pointer);
    }
    kfree(image);
    if (status == K_OK) {
        status = thread_create_user_suspended(process, info.entry,
                                              stack_pointer, 0U, 0U,
                                              &thread);
    }
    if (status == K_OK) {
        status = thread_start(thread);
    }
    if (status != K_OK) {
        if (thread != 0) {
            (void)thread_terminate(thread, status);
            object_put(thread);
        }
        if (process != 0) {
            if (thread == 0) (void)process_abort(process);
            object_put(process);
        }
        if (chrome_launch_claimed) {
            (void)desktop_chrome_launch_release(
                &g_window_server.desktop_chrome_launch_pid,
                chrome_launch_token);
        }
        return status;
    }

    object_put(thread);
    object_put(process);
    return K_OK;
}

#endif
