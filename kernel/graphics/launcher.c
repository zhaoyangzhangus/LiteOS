#include <kernel/elf_loader.h>
#include <kernel/kmem.h>
#include <kernel/sched.h>
#include <kernel/vfs.h>
#include "internal.h"

#ifdef LITEOS_KERNEL_BUILD

/* REFACTOR_P7A_SHELL_LAUNCHER_OWNER: desktop app launch and title policy. */

#define DESKTOP_PROGRAM_MAX_BYTES (16ULL * 1024ULL * 1024ULL)

const char *window_shell_program_path(uint32_t app) {
    if (app == DESKTOP_APP_FILES) return "/sbin/fileman";
    if (app == DESKTOP_APP_TERMINAL) return "/sbin/gshell";
    if (app == DESKTOP_APP_NOTES) return "/sbin/notepad";
    if (app == DESKTOP_APP_NETWORK) return "/sbin/netmgr";
    if (app == DESKTOP_APP_TASKMGR) return "/sbin/taskmgr";
    return 0;
}

const char *window_shell_app_title(uint32_t app) {
    if (app == DESKTOP_APP_FILES) return "FILEMAN";
    if (app == DESKTOP_APP_TERMINAL) return "SHELL";
    if (app == DESKTOP_APP_NOTES) return "NOTEPAD";
    if (app == DESKTOP_APP_NETWORK) return "NETWORK";
    if (app == DESKTOP_APP_TASKMGR) return "TASK MANAGER";
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

bool desktop_restore_minimized_app(uint32_t app) {
    const char *expected = window_shell_app_title(app);
    window_server_window_t *window = 0;
    if (expected == 0) return false;

    window_lock();
    for (uint32_t index = g_window_server.count; index != 0U; --index) {
        window_server_window_t *candidate =
            g_window_server.windows[index - 1U];
        if (candidate == 0 || !candidate->minimized ||
            (candidate->flags & OS_WINDOW_VISIBLE) == 0U ||
            !window_shell_title_matches(candidate->title, expected)) {
            continue;
        }
        window = candidate;
        break;
    }
    if (window == 0) {
        window_unlock();
        return false;
    }

    window->minimized = false;
    window->dirty = true;
    window_scene_focus_locked(window);
    window_mark_window_locked(window);
    window_unlock();
    (void)wake_all(&g_window_server.event_waitq);
    return true;
}

kstatus_t desktop_launch_program(uint32_t app) {
    const char *path = window_shell_program_path(app);
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
    status = vfs_open_kernel(path, VFS_OPEN_READ, 0U, &file);
    if (status != K_OK) return status;
    if (file->vnode == 0 || file->vnode->size == 0U ||
        file->vnode->size > DESKTOP_PROGRAM_MAX_BYTES) {
        vfs_close(file);
        return K_EINVAL;
    }

    image_size = file->vnode->size;
    image = (uint8_t *)kmalloc((size_t)image_size, 0);
    if (image == 0) {
        vfs_close(file);
        return K_ENOMEM;
    }
    status = vfs_read_kernel(file, image, image_size, &bytes);
    vfs_close(file);
    if (status != K_OK || bytes != image_size) {
        kfree(image);
        return status == K_OK ? K_EIO : status;
    }

    status = process_create(0, &process);
    if (status == K_OK) {
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
    if (status == K_OK) status = thread_start(thread);
    if (status != K_OK) {
        if (thread != 0) {
            (void)thread_terminate(thread, status);
            object_put(thread);
        }
        if (process != 0) {
            if (thread == 0) (void)process_abort(process);
            object_put(process);
        }
        return status;
    }

    object_put(thread);
    object_put(process);
    return K_OK;
}

#endif
