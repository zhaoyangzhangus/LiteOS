#include <arch/x86_64/context.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/smp.h>
#include <kernel/console.h>
#include <kernel/kmem.h>
#include <kernel/sched.h>
#include <kernel/vfs.h>
#include "internal.h"

/* REFACTOR_P7A_SHELL_ASSETS_OWNER: cold desktop asset I/O and worker policy. */

#define WINDOW_DESKTOP_ASSET_WORKER_STACK_SIZE (32U * 1024U)
#define DESKTOP_ASSET_MAX_BYTES               (16ULL * 1024ULL * 1024ULL)

static wait_queue_t g_desktop_asset_waitq;
static uint8_t g_desktop_asset_worker_stack
    [WINDOW_DESKTOP_ASSET_WORKER_STACK_SIZE]
    __attribute__((aligned(16)));

atomic_bool g_desktop_assets_available;
atomic_bool g_desktop_assets_pending;
desktop_asset_image_t g_desktop_wallpaper_asset;
desktop_asset_image_t g_desktop_icons_asset;
desktop_asset_image_t g_desktop_file_manager_asset;

void desktop_shell_init(void) {
    wait_queue_init(&g_desktop_asset_waitq);
    atomic_init(&g_desktop_assets_available, false);
    atomic_init(&g_desktop_assets_pending, false);
    g_desktop_wallpaper_asset = (desktop_asset_image_t){0};
    g_desktop_icons_asset = (desktop_asset_image_t){0};
    g_desktop_file_manager_asset = (desktop_asset_image_t){0};
}

bool desktop_shell_assets_available(void) {
    return atomic_load_explicit(&g_desktop_assets_available,
                                memory_order_acquire);
}

static bool desktop_load_png_asset(const char *path,
                                   desktop_asset_image_t *asset) {
    file_t *file = 0;
    uint8_t *encoded = 0;
    uint64_t bytes_read = 0U;
    uint64_t file_size;
    kstatus_t status;
    bool loaded;

    if (path == 0 || asset == 0 || asset->storage != 0) return false;
    status = vfs_open_kernel(path, VFS_OPEN_READ, 0U, &file);
    if (status != K_OK || file == 0 || file->vnode == 0) {
        if (file != 0) vfs_close(file);
        return false;
    }
    file_size = file->vnode->size;
    if (file_size == 0U || file_size > DESKTOP_ASSET_MAX_BYTES ||
        file_size > SIZE_MAX) {
        vfs_close(file);
        return false;
    }
    encoded = (uint8_t *)kmalloc((size_t)file_size, 0U);
    if (encoded == 0) {
        vfs_close(file);
        return false;
    }
    status = vfs_read_kernel(file, encoded, (size_t)file_size, &bytes_read);
    vfs_close(file);
    if (status != K_OK || bytes_read != file_size) {
        kfree(encoded);
        return false;
    }
    loaded = desktop_png_decode(encoded, (size_t)file_size, asset);
    kfree(encoded);
    return loaded;
}

static bool desktop_load_assets(void) {
    bool wallpaper = desktop_load_png_asset(
        "/etc/desktop/wall.png",
        &g_desktop_wallpaper_asset);
    bool icons = desktop_load_png_asset(
        "/etc/desktop/icons.png",
        &g_desktop_icons_asset);
    bool file_manager = desktop_load_png_asset(
        "/etc/desktop/fm.png",
        &g_desktop_file_manager_asset);
    return wallpaper && icons && file_manager;
}

static bool desktop_asset_worker_never_ready(void *context) {
    (void)context;
    return false;
}

/* Keep multi-megabyte artwork reads off the compositor worker CPU. */
static void __attribute__((noreturn)) desktop_asset_worker_main(void *argument) {
    bool loaded;
    (void)argument;

    loaded = desktop_load_assets();
    if (loaded) {
        liteos_serial_write_serial_only("LITEOS_DESKTOP_ASSETS_OK\r\n");
        atomic_store_explicit(&g_desktop_assets_available, true,
                              memory_order_release);
        atomic_store_explicit(&g_desktop_assets_pending, true,
                              memory_order_release);
        window_lock();
        window_mark_dirty_locked();
        window_unlock();
        window_server_notify_worker();
    } else {
        liteos_serial_write_serial_only("LITEOS_DESKTOP_ASSETS_FAIL\r\n");
    }

    /* Keep the scheduler-visible thread blocked after its one-shot job. */
    for (;;) {
        (void)wait_on_queue(&g_desktop_asset_waitq,
                            desktop_asset_worker_never_ready,
                            0, UINT64_MAX);
    }
}

bool desktop_shell_start_asset_worker(uint32_t compositor_cpu) {
    uint32_t current_cpu = x86_current_cpu_index();
    uint32_t asset_cpu = UINT32_MAX;
    thread_t *worker;
    uint8_t *worker_bytes;

    if (atomic_load_explicit(&g_window_server.desktop_asset_worker_started,
                             memory_order_acquire)) {
        return true;
    }

    /* Prefer a third CPU so the potentially long VFS read cannot compete
     * with either the compositor or the bootstrap CPU. */
    for (uint32_t candidate = 0U;
         candidate < x86_smp_discovered_count() && candidate < MAX_CPUS;
         ++candidate) {
        if (candidate == current_cpu || candidate == compositor_cpu ||
            !x86_smp_cpu_online(candidate)) {
            continue;
        }
        asset_cpu = candidate;
        break;
    }
    if (asset_cpu == UINT32_MAX) {
        /* A one-CPU guest still needs the artwork; run this low-priority job
         * on the bootstrap CPU when no spare CPU is available. */
        asset_cpu = current_cpu;
    }

    worker = &g_window_server.desktop_asset_worker;
    worker_bytes = (uint8_t *)worker;
    for (size_t byte = 0U; byte < sizeof(*worker); ++byte) {
        worker_bytes[byte] = 0U;
    }

    refcount_init(&worker->object.refs, 1U);
    worker->object.type = KOBJECT_TYPE_THREAD;
    worker->object.flags = 0U;
    worker->object.ops = 0;
    worker->tid = UINT64_MAX - 4ULL;
    worker->process = 0;
    atomic_init(&worker->state, THREAD_READY);
    atomic_init(&worker->block_epoch, 0U);
        atomic_init(&worker->command_ack, 0U);
    worker->kernel_stack_base = g_desktop_asset_worker_stack;
    worker->kernel_stack_size = sizeof(g_desktop_asset_worker_stack);
    worker->kernel_stack_top =
        ((vaddr_t)(uintptr_t)g_desktop_asset_worker_stack +
         sizeof(g_desktop_asset_worker_stack)) & ~(vaddr_t)0x0FULL;
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
    worker->affinity.bits[asset_cpu >> 6] = 1ULL << (asset_cpu & 63U);
    worker->owner_cpu = (uint16_t)asset_cpu;
    worker->current_cpu = (uint16_t)asset_cpu;

    uintptr_t stack_top = (uintptr_t)worker->kernel_stack_top;
    uintptr_t switch_stack = stack_top - sizeof(uint64_t);
    *(uint64_t *)switch_stack =
        (uint64_t)(uintptr_t)&x86_kernel_thread_start;
    worker->arch.switch_ctx.rsp = switch_stack;
    worker->arch.switch_ctx.r12 =
        (uint64_t)(uintptr_t)&desktop_asset_worker_main;
    worker->arch.switch_ctx.r13 = 0U;
    worker->arch.switch_ctx.r14 = stack_top;
    worker->arch.fs_base = 0U;

    atomic_store_explicit(&g_window_server.desktop_asset_worker_started, true,
                          memory_order_release);
    sched_enqueue_bootstrap(worker);
    return true;
}
