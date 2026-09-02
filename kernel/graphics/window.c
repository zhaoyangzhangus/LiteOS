#include <arch/x86_64/paging.h>
#include <arch/x86_64/cpu.h>
#include <kernel/console.h>
#include <kernel/kmem.h>
#include <kernel/perf.h>
#include <kernel/telemetry.h>
#include <kernel/vm.h>
#include "internal.h"

/* REFACTOR_P7A_WINDOW_OWNER: registry lifetime, removal, and public window API. */

void window_object_destroy(void *raw_object) {
    window_server_window_t *window = (window_server_window_t *)raw_object;
    if (window == 0) return;

    if (window->compositor_cache != 0) {
        kfree(window->compositor_cache);
        window->compositor_cache = 0;
    }

    if (window->section != 0) {
        object_put(window->section);
        window->section = 0;
    }
    if (window->owner != 0) {
        object_put(window->owner);
        window->owner = 0;
    }
    kfree(window);
}

static void window_handle_close(void *object) {
    window_server_handle_closed((window_server_window_t *)object);
}

static uint64_t window_tsc_to_us(uint64_t elapsed_tsc) {
    if (x86_boot_cpu_features.tsc_hz == 0U) return 0U;
    return x86_tsc_to_ns(elapsed_tsc) / 1000U;
}

uint64_t window_surface_page_span(uint32_t width, uint32_t height) {
    uint64_t pixels = (uint64_t)width * height;
    uint64_t bytes;

    if (width == 0U || height == 0U ||
        pixels > UINT64_MAX / sizeof(uint32_t)) return 0U;
    bytes = pixels * sizeof(uint32_t);
    if (bytes > UINT64_MAX - (PAGE_SIZE - 1ULL)) return 0U;
    return (bytes + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
}

const object_ops_t g_window_object_ops = {
    .destroy = window_object_destroy,
    .handle_close = window_handle_close,
    .type_name = "Window",
    .is_signaled = 0,
    .wait_value = 0,
};

void window_registry_reset_locked(void) {
    g_window_server.count = 0U;
    g_window_server.next_identifier = 1U;

    for (uint32_t index = 0U;
         index < WINDOW_SERVER_MAX_WINDOWS;
         ++index) {
        g_window_server.windows[index] = 0;
    }
}

bool window_registry_append_locked(window_server_window_t *window) {
    uint32_t identifier;

    if (window == 0 ||
        g_window_server.count >= WINDOW_SERVER_MAX_WINDOWS) {
        return false;
    }

    identifier = g_window_server.next_identifier++;
    if (identifier == 0U) {
        identifier = g_window_server.next_identifier++;
    }

    window->identifier = identifier;
    g_window_server.windows[g_window_server.count++] = window;
    return true;
}

bool window_registry_remove_locked(window_server_window_t *window) {
    if (window == 0) {
        return false;
    }

    for (uint32_t index = 0U;
         index < g_window_server.count;
         ++index) {
        if (g_window_server.windows[index] != window) {
            continue;
        }

        for (uint32_t move = index + 1U;
             move < g_window_server.count;
             ++move) {
            g_window_server.windows[move - 1U] =
                g_window_server.windows[move];
        }

        --g_window_server.count;
        g_window_server.windows[g_window_server.count] = 0;
        return true;
    }

    return false;
}

void window_registry_move_to_front_locked(window_server_window_t *window) {
    uint32_t position;

    if (window == 0) {
        return;
    }

    for (position = 0U;
         position < g_window_server.count;
         ++position) {
        if (g_window_server.windows[position] == window) {
            break;
        }
    }

    if (position >= g_window_server.count ||
        position + 1U >= g_window_server.count) {
        return;
    }

    for (uint32_t index = position + 1U;
         index < g_window_server.count;
         ++index) {
        g_window_server.windows[index - 1U] =
            g_window_server.windows[index];
    }

    g_window_server.windows[g_window_server.count - 1U] = window;
}

static bool window_lifecycle_test_fail(uint32_t step, kstatus_t status) {
    uint32_t code = status < 0 ? (uint32_t)(-status) : (uint32_t)status;
    liteos_serial_write("LITEOS_WINDOW_TEST_FAIL STEP=");
    liteos_serial_write_u32(step);
    liteos_serial_write(" STATUS=");
    liteos_serial_write_u32(code);
    liteos_serial_write("\r\n");
    return false;
}

/* REFACTOR_P7A_WINDOW_TEST_OWNER: canonical lifecycle and registry coverage. */
bool window_lifecycle_self_test(void) {
    process_t *owner = 0;
    window_server_window_t *first = 0;
    window_server_window_t *second = 0;
    window_server_snapshot_t snapshot = {0};
    uint64_t first_address = 0U;
    uint64_t second_address = 0U;
    uint64_t initial_pages;
    uint64_t resize_pages_before;
    uint64_t resize_pages_after;
    uint32_t initial_count = 0U;
    uint32_t dirty_rects;
    uint64_t benchmark_start;
    input_event_t pointer_event = {0};
    kstatus_t status;
    bool success = false;
    bool count_restored = false;

    if (process_create(0, &owner) != K_OK) {
        success = window_lifecycle_test_fail(1U, K_EIO);
        goto cleanup;
    }
    if (!window_server_kernel_ready()) {
        success = window_lifecycle_test_fail(2U, K_EIO);
        goto cleanup;
    }

    window_lock();
    initial_count = g_window_server.count;
    window_unlock();

    if (!compositor_copy_self_test()) {
        success = window_lifecycle_test_fail(3U, K_EIO);
        goto cleanup;
    }

    compositor_snapshot_test_clear_damage_tiles();
    if (!compositor_snapshot_test_set_damage_tile(0U)) {
        success = window_lifecycle_test_fail(4U, K_EIO);
        goto cleanup;
    }
    benchmark_start = telemetry_timestamp();
    dirty_rects = compositor_snapshot_tiles_to_rects(
        &g_compositor_snapshot,
        WINDOW_DAMAGE_MAX_SNAPSHOT_RECTS);
    kernel_perf_emit_scope("graphics.single_dirty_tile", benchmark_start);
    kernel_perf_emit_value("graphics.dirty_tiles_frame", dirty_rects);
    compositor_snapshot_test_clear_damage_tiles();
    if (dirty_rects != 1U) {
        success = window_lifecycle_test_fail(4U, K_EIO);
        goto cleanup;
    }

    benchmark_start = telemetry_timestamp();
    if (window_server_create(owner, 8, 8, 96U, 56U,
                             OS_WINDOW_VISIBLE, 0x00102030U,
                             "back", &first) != K_OK) {
        success = window_lifecycle_test_fail(3U, K_EIO);
        goto cleanup;
    }
    if (window_server_map(first, owner, 0U, &first_address) != K_OK ||
        window_server_set_owner_address(first, first_address) != K_OK ||
        window_server_set(first, 8, 8, 1U) != K_OK) {
        success = window_lifecycle_test_fail(3U, K_EIO);
        goto cleanup;
    }
    kernel_perf_emit_value("window_create_us",
                           window_tsc_to_us(telemetry_timestamp() -
                                            benchmark_start));
    initial_pages = vm_object_populated_pages(first->section->vm_object);
    if (initial_pages != window_surface_page_span(96U, 56U) / PAGE_SIZE) {
        success = window_lifecycle_test_fail(3U, K_EIO);
        goto cleanup;
    }
    benchmark_start = telemetry_timestamp();
    compositor_frame_run();
    kernel_perf_emit_scope("graphics.single_window_compose", benchmark_start);

    benchmark_start = telemetry_timestamp();
    if (window_server_create(owner, 48, 24, 96U, 56U,
                             OS_WINDOW_VISIBLE | OS_WINDOW_RESIZABLE,
                             0x00203040U,
                             "front", &second) != K_OK) {
        success = window_lifecycle_test_fail(4U, K_EIO);
        goto cleanup;
    }
    if (window_server_map(second, owner, 0U, &second_address) != K_OK ||
        window_server_set_owner_address(second, second_address) != K_OK ||
        window_server_set(second, 48, 24, 1U) != K_OK) {
        success = window_lifecycle_test_fail(4U, K_EIO);
        goto cleanup;
    }
    kernel_perf_emit_value("window_create_us",
                           window_tsc_to_us(telemetry_timestamp() -
                                            benchmark_start));
    second->surface_metrics_reported = true;

    benchmark_start = telemetry_timestamp();
    compositor_frame_run();
    kernel_perf_emit_scope("graphics.overlap_2", benchmark_start);

    if (window_server_snapshot(initial_count, &snapshot) != K_OK ||
        snapshot.identifier != first->identifier ||
        snapshot.owner_pid != (uint32_t)owner->pid ||
        snapshot.x != 8 || snapshot.y != 8 ||
        snapshot.width != 96U || snapshot.height != 56U ||
        snapshot.visible == 0U || snapshot.title[0] != 'b') {
        success = window_lifecycle_test_fail(5U, K_EIO);
        goto cleanup;
    }
    if (window_server_snapshot(initial_count + 1U, &snapshot) != K_OK ||
        snapshot.identifier != second->identifier ||
        snapshot.focused == 0U || snapshot.title[0] != 'f') {
        success = window_lifecycle_test_fail(6U, K_EIO);
        goto cleanup;
    }
    if (window_server_focus(first->identifier) != K_OK) {
        success = window_lifecycle_test_fail(7U, K_EIO);
        goto cleanup;
    }
    benchmark_start = telemetry_timestamp();
    status = window_server_set(first, 21, 17, 1U);
    kernel_perf_emit_scope("graphics.window_move_latency", benchmark_start);
    if (status != K_OK) {
        success = window_lifecycle_test_fail(8U, K_EIO);
        goto cleanup;
    }
    if (window_server_update(owner, first->identifier, 21, 17,
                             96U, 56U, 0U) != K_OK) {
        success = window_lifecycle_test_fail(9U, K_EIO);
        goto cleanup;
    }
    resize_pages_before = vm_object_populated_pages(second->section->vm_object);
    window_lock();
    g_window_server.resize_edges = WINDOW_RESIZE_RIGHT | WINDOW_RESIZE_BOTTOM;
    bool resized = window_resize_locked(second, 200, 100);
    g_window_server.resize_edges = 0U;
    window_unlock();
    if (!resized) {
        success = window_lifecycle_test_fail(10U, K_EIO);
        goto cleanup;
    }
    window_surface_populate_pending();
    resize_pages_after = vm_object_populated_pages(second->section->vm_object);
    if (resize_pages_after <= resize_pages_before ||
        window_server_update(owner, second->identifier, 0, 0,
                             second->width, second->height, 0U) != K_OK) {
        success = window_lifecycle_test_fail(10U, K_EIO);
        goto cleanup;
    }
    kernel_perf_emit_value("window_test_initial_pages", initial_pages);
    kernel_perf_emit_value("window_test_resize_new_pages",
                           resize_pages_after - resize_pages_before);
    if (window_server_snapshot(initial_count + 1U, &snapshot) != K_OK ||
        snapshot.identifier != first->identifier ||
        snapshot.x != 21 || snapshot.y != 17 ||
        snapshot.focused == 0U) {
        success = window_lifecycle_test_fail(10U, K_EIO);
        goto cleanup;
    }

    pointer_event.device_id = 0xC0DEU;
    pointer_event.type = INPUT_EVENT_POINTER;
    pointer_event.code = 0U;
    pointer_event.value2 = 1;
    pointer_event.value3 = 0;
    pointer_event.value4 = 0;
    benchmark_start = telemetry_timestamp();
    status = input_core_push(&pointer_event);
    if (status != K_OK) {
        success = window_lifecycle_test_fail(13U, status);
        goto cleanup;
    }
    window_server_pump_input_mode(true);
    kernel_perf_emit_scope("graphics.input_present", benchmark_start);

    if (window_server_set(second, 48, 24, 0U) != K_OK) {
        success = window_lifecycle_test_fail(11U, K_EIO);
        goto cleanup;
    }
    if (window_server_snapshot(initial_count, &snapshot) != K_OK ||
        snapshot.identifier != second->identifier ||
        snapshot.visible != 0U) {
        success = window_lifecycle_test_fail(12U, K_EIO);
        goto cleanup;
    }

    success = true;

cleanup:
    if (second != 0) {
        window_server_handle_closed(second);
        window_server_put(second);
    }
    if (first != 0) {
        window_server_handle_closed(first);
        window_server_put(first);
    }
    if (owner != 0) {
        window_lock();
        count_restored = g_window_server.count == initial_count;
        window_unlock();
        (void)process_abort(owner);
        object_put(owner);
    }
    if (!count_restored) {
        (void)window_lifecycle_test_fail(13U, K_EIO);
        return false;
    }
    return success;
}

void remove_window_locked(window_server_window_t *window) {
    window_server_window_t *new_focused = 0;

    if (window == 0 ||
        !window_registry_remove_locked(window)) {
        return;
    }

    window_mark_window_locked(window);

    if (g_window_server.focused_identifier == window->identifier) {
        window_scene_set_focus_identifier_locked(0U);
        for (uint32_t j = g_window_server.count; j != 0U; --j) {
            window_server_window_t *candidate =
                g_window_server.windows[j - 1U];
            if (!candidate->minimized &&
                (candidate->flags & OS_WINDOW_VISIBLE) != 0U) {
                window_scene_set_focus_identifier_locked(candidate->identifier);
                new_focused = candidate;
                break;
            }
        }
    }

    if (g_window_server.title_pressed_identifier ==
        window->identifier) {
        window_clear_title_capture_locked();
    }

    if (g_window_server.dragging_identifier == window->identifier) {
        window_input_clear_drag_locked();
    }

    window_mark_window_locked(new_focused);
    object_put(window); /* registry reference */
}


kstatus_t window_server_register_manager(process_t *process) {
    (void)process;
    /* 合成和输入路由已经属于 Ring0，用户进程不能接管窗口服务器。 */
    return window_server_kernel_ready() ? K_EPERM : K_EIO;
}

bool window_server_is_manager(process_t *process) {
    (void)process;
    return false;
}

kstatus_t window_server_create(process_t *owner, int32_t x, int32_t y,
                               uint32_t width, uint32_t height,
                               uint32_t flags, uint32_t background,
                               const char *title, window_server_window_t **out) {
    uint64_t pixels;
    uint64_t bytes;
    uint64_t create_tsc = telemetry_timestamp();
    shared_section_t *section = 0;
    window_server_window_t *window;
    window_server_window_t *old_focused;

    if (out == 0 || owner == 0 || !window_server_init() || width == 0U ||
        height == 0U || width > UINT32_MAX / height ||
        (flags & ~(OS_WINDOW_VISIBLE | OS_WINDOW_RESIZABLE |
                   OS_WINDOW_CLIENT_DECORATIONS)) != 0U) {
        return K_EINVAL;
    }

    /*
     * Resizable windows get a stable virtual surface mapping large enough for
     * any on-screen size.  VM_OBJECT_SHARED is demand-paged, so this reserves
     * virtual capacity without eagerly allocating a full-screen physical
     * buffer.
     */
    if ((flags & OS_WINDOW_RESIZABLE) != 0U) {
        if (!window_server_kernel_ready()) return K_EIO;
        if (width > g_window_server.display_width ||
            height > g_window_server.display_height) {
            return K_EINVAL;
        }
        pixels = (uint64_t)g_window_server.display_width *
                 g_window_server.display_height;
    } else {
        pixels = (uint64_t)width * height;
    }

    if (pixels > UINT64_MAX / sizeof(uint32_t)) return K_EINVAL;
    bytes = pixels * sizeof(uint32_t);
    if (bytes > UINT64_MAX - (PAGE_SIZE - 1U)) return K_EINVAL;
    bytes = (bytes + PAGE_SIZE - 1U) &
            ~(uint64_t)(PAGE_SIZE - 1U);

    if (shared_section_create(bytes, &section) != K_OK) return K_ENOMEM;
    vm_object_mark_window_surface(section->vm_object);
    window = (window_server_window_t *)kzalloc(sizeof(*window), 0);
    if (window == 0) {
        object_put(section);
        return K_ENOMEM;
    }
    refcount_init(&window->object.refs, 1U);
    window->object.type = KOBJECT_TYPE_WINDOW;
    window->object.flags = 0U;
    window->object.ops = &g_window_object_ops;
    window->owner = owner;
    object_get(owner);
    window->section = section;
    window->x = x;
    window->y = y;
    window->width = width;
    window->height = height;
    /* Publish only after the caller has mapped and populated the surface. */
    window->flags = flags & ~OS_WINDOW_VISIBLE;
    window->background = background;
    window->buffer_size = bytes;
    window->owner_address = 0U;
    window->create_tsc = create_tsc;
    window->surface_fault_count_at_create =
        vm_object_fault_count(section->vm_object);
    window->surface_populated_bytes = 0U;
    window->surface_populate_pending_end = 0U;
    window->surface_metrics_reported = false;
    window->compositor_cache = 0;
    window->compositor_presented_x = x;
    window->compositor_presented_y = y;
    window->compositor_presented_width = 0U;
    window->compositor_presented_height = 0U;
    window->compositor_presented_valid = false;
    window->dirty = true;
    window->resize_pending = false;
    window->maximized = false;
    window->minimized = false;
    window->restore_x = x;
    window->restore_y = y;
    window->restore_width = width;
    window->restore_height = height;
    window->event_read = 0U;
    window->event_write = 0U;
    window->event_count = 0U;
    window->event_wake_pending = false;
    wait_queue_init(&window->event_waitq);
    for (uint32_t i = 0U; i + 1U < sizeof(window->title); ++i) {
        window->title[i] = title != 0 ? title[i] : '\0';
        if (window->title[i] == '\0') break;
    }
    window->title[sizeof(window->title) - 1U] = '\0';
    window_lock();
    old_focused = window_scene_find_locked(g_window_server.focused_identifier);
    if (!window_registry_append_locked(window)) {
        window_unlock();
        object_put(window);
        return K_ENOMEM;
    }
    object_get(window); /* registry reference */
    window_mark_window_locked(old_focused);
    window_mark_window_locked(window);
    window_unlock();
    window_server_notify_worker();
    *out = window;
    return K_OK;
}

kstatus_t window_server_lookup(uint32_t identifier, window_server_window_t **out) {
    window_server_window_t *window;
    if (out == 0 || !window_server_init()) return K_EINVAL;
    window_lock();
    window = window_scene_find_locked(identifier);
    if (window == 0 || !object_try_get(window)) {
        window_unlock();
        return K_ENOENT;
    }
    window_unlock();
    *out = window;
    return K_OK;
}

uint32_t window_server_window_identifier(
    const window_server_window_t *window) {
    return window != 0 ? window->identifier : 0U;
}

uint64_t window_server_window_buffer_size(
    const window_server_window_t *window) {
    return window != 0 ? window->buffer_size : 0U;
}

void window_server_put(window_server_window_t *window) {
    object_put(window);
}

void window_server_handle_closed(window_server_window_t *window) {
    bool referenced = false;
    if (window == 0 || !window_server_init()) return;
    window_lock();
    object_get(window);
    referenced = true;
    remove_window_locked(window);
    window_unlock();
    window_server_notify_worker();
    (void)wake_all(&window->event_waitq);
    if (referenced) object_put(window);
}

void window_server_close_process(process_t *owner) {
    window_server_window_t *removed[WINDOW_SERVER_MAX_WINDOWS];
    uint32_t removed_count = 0U;
    if (owner == 0 || !window_server_init()) return;
    window_lock();
    for (uint32_t index = 0U; index < g_window_server.count;) {
        window_server_window_t *window = g_window_server.windows[index];
        if (window == 0 || window->owner != owner) {
            ++index;
            continue;
        }
        /* remove_window_locked() compacts the array, so keep the index. */
        object_get(window);
        remove_window_locked(window);
        if (removed_count < WINDOW_SERVER_MAX_WINDOWS) {
            removed[removed_count++] = window;
        } else {
            object_put(window);
        }
    }
    window_unlock();
    for (uint32_t index = 0U; index < removed_count; ++index) {
        (void)wake_all(&removed[index]->event_waitq);
        object_put(removed[index]);
    }
    if (removed_count != 0U) window_server_notify_worker();
}

kstatus_t window_server_set_owner_address(window_server_window_t *window,
                                          uint64_t address) {
    if (window == 0 || address == 0U || (address & (PAGE_SIZE - 1U)) != 0U) {
        return K_EINVAL;
    }
    window_lock();
    window->owner_address = address;
    window_mark_window_locked(window);
    window_unlock();
    window_server_notify_worker();
    return K_OK;
}

kstatus_t window_server_snapshot(uint32_t index, window_server_snapshot_t *out) {
    window_server_window_t *window;
    if (out == 0 || !window_server_init()) return K_EINVAL;
    window_lock();
    if (index >= g_window_server.count) {
        window_unlock();
        return K_ENOENT;
    }
    window = g_window_server.windows[index];
    out->identifier = window->identifier;
    out->owner_pid = window->owner != 0 ? (uint32_t)window->owner->pid : 0U;
    out->x = window->x;
    out->y = window->y;
    out->width = window->width;
    out->height = window->height;
    out->visible = (window->flags & OS_WINDOW_VISIBLE) != 0U;
    out->focused = window->identifier == g_window_server.focused_identifier;
    out->z_order = index;
    out->buffer_size = window->buffer_size;
    for (uint32_t i = 0U; i < sizeof(out->title); ++i) out->title[i] = window->title[i];
    window_unlock();
    return K_OK;
}

kstatus_t window_server_set(window_server_window_t *window, int32_t x, int32_t y,
                            uint32_t visible) {
    window_server_window_t *old_focused;
    int32_t old_x;
    int32_t old_y;
    if (window == 0 || (visible & ~1U) != 0U) return K_EINVAL;
    window_lock();
    old_x = window->x;
    old_y = window->y;
    old_focused = window_scene_find_locked(g_window_server.focused_identifier);
    window_mark_moved_rect_locked(old_x, old_y, x, y,
                                  window_outer_width(window->width, window->flags),
                                  window_outer_height(window->height, window->flags),
                                  true);
    window->x = x;
    window->y = y;
    if (visible != 0U) {
        window->flags |= OS_WINDOW_VISIBLE;
        window_scene_set_focus_identifier_locked(window->identifier);
    } else {
        window->flags &= ~OS_WINDOW_VISIBLE;
    }
    if (visible == 0U && g_window_server.focused_identifier == window->identifier) {
        window_scene_set_focus_identifier_locked(0U);
        for (uint32_t i = g_window_server.count; i != 0U; --i) {
            window_server_window_t *candidate = g_window_server.windows[i - 1U];
            if ((candidate->flags & OS_WINDOW_VISIBLE) != 0U) {
                window_scene_set_focus_identifier_locked(candidate->identifier);
                break;
            }
        }
    }
    window_mark_window_locked(old_focused);
    window_mark_window_locked(window);
    window_unlock();
    window_server_notify_worker();
    return K_OK;
}

kstatus_t window_server_focus(uint32_t identifier) {
    window_server_window_t *window;
    if (!window_server_init()) return K_EINVAL;
    window_lock();
    window = window_scene_find_locked(identifier);
    if (window == 0 || (window->flags & OS_WINDOW_VISIBLE) == 0U) {
        window_unlock();
        return K_ENOENT;
    }
    window_scene_focus_locked(window);
    window_unlock();
    window_server_notify_worker();
    return K_OK;
}

kstatus_t window_server_map(window_server_window_t *window, process_t *process,
                            uint64_t requested_address, uint64_t *mapped_address) {
    vm_object_t *object;
    vaddr_t address;
    kstatus_t status;
    uint64_t populate_start;
    uint64_t populate_length;
    uint64_t populate_tsc;
    if (window == 0 || process == 0 || mapped_address == 0 ||
        window->section == 0 || process->vm == 0) return K_EINVAL;
    object = window->section->vm_object;
    if (object == 0) return K_EIO;
    vm_object_get(object);
    address = (vaddr_t)requested_address;
    status = vm_map_object(process->vm, object, &address, 0U,
                           (size_t)window->buffer_size,
                           VM_PROT_USER | VM_PROT_READ | VM_PROT_WRITE,
                           VM_MAP_SHARED);
    vm_object_put(object);
    if (status != K_OK) return status;

    populate_start = window_surface_page_span(window->width, window->height);
    if (populate_start == 0U || populate_start > window->buffer_size) {
        (void)vm_unmap(process->vm, address, (size_t)window->buffer_size);
        return K_EINVAL;
    }
    populate_tsc = telemetry_timestamp();
    status = vm_populate_range(process->vm, address,
                               (size_t)populate_start);
    if (status != K_OK) {
        (void)vm_unmap(process->vm, address, (size_t)window->buffer_size);
        return status;
    }
    populate_length = populate_start;
    window_lock();
    if (populate_length > window->surface_populated_bytes) {
        window->surface_populated_bytes = populate_length;
    }
    window_unlock();
    kernel_perf_emit_scope("window.surface_populate", populate_tsc);
    kernel_perf_emit_value(
        "surface_populate_us",
        window_tsc_to_us(telemetry_timestamp() - populate_tsc));
    *mapped_address = (uint64_t)address;
    return K_OK;
}

void window_surface_populate_pending(void) {
    typedef struct window_surface_request {
        window_server_window_t *window;
        uint64_t address;
        uint64_t start;
        uint64_t end;
    } window_surface_request_t;
    window_surface_request_t requests[WINDOW_SERVER_MAX_WINDOWS];
    uint32_t request_count = 0U;

    window_lock();
    for (uint32_t index = 0U;
         index < g_window_server.count &&
             request_count < WINDOW_SERVER_MAX_WINDOWS;
         ++index) {
        window_server_window_t *window = g_window_server.windows[index];
        if (window == 0 || window->owner_address == 0U ||
            window->surface_populate_pending_end <=
                window->surface_populated_bytes) {
            continue;
        }
        object_get(window);
        requests[request_count++] = (window_surface_request_t){
            .window = window,
            .address = window->owner_address,
            .start = window->surface_populated_bytes,
            .end = window->surface_populate_pending_end,
        };
    }
    window_unlock();

    for (uint32_t index = 0U; index < request_count; ++index) {
        window_surface_request_t *request = &requests[index];
        uint64_t start_tsc = telemetry_timestamp();
        process_t *owner = request->window->owner;
        kstatus_t status = owner == 0 || owner->vm == 0 ? K_EINVAL :
            vm_populate_range(owner->vm,
                              (vaddr_t)(request->address + request->start),
                              (size_t)(request->end - request->start));
        if (status == K_OK) {
            window_lock();
            if (request->end > request->window->surface_populated_bytes) {
                request->window->surface_populated_bytes = request->end;
            }
            if (request->window->surface_populate_pending_end <=
                    request->window->surface_populated_bytes) {
                request->window->surface_populate_pending_end = 0U;
            }
            window_unlock();
            kernel_perf_emit_scope("window.resize_populate", start_tsc);
        }
        object_put(request->window);
    }
}

kstatus_t window_server_dispatch(uint32_t identifier, const os_input_event_t *event) {
    window_server_window_t *window;
    if (event == 0 || !window_server_init()) return K_EINVAL;
    window_lock();
    window = window_scene_find_locked(identifier);
    if (window == 0 || window->owner == 0) {
        window_unlock();
        return K_ENOENT;
    }
    if (window->event_count >= WINDOW_EVENT_CAPACITY) {
        window->event_read = (window->event_read + 1U) % WINDOW_EVENT_CAPACITY;
        --window->event_count;
    }
    window->events[window->event_write].identifier = identifier;
    window->events[window->event_write].type = OS_WINDOW_EVENT_INPUT;
    window->events[window->event_write].input = *event;
    window->event_write = (window->event_write + 1U) % WINDOW_EVENT_CAPACITY;
    ++window->event_count;
    window_event_schedule_wake_locked(window);
    window_unlock();

    if (window_flush_event_wakes()) {
        (void)wake_all(
            &g_window_server.event_waitq);
    }

    return K_OK;
}

typedef struct window_event_wait_context {
    process_t *process;
    uint32_t identifier;
    os_window_event_t *event;
} window_event_wait_context_t;

static bool window_event_available(void *raw_context) {
    window_event_wait_context_t *context =
        (window_event_wait_context_t *)raw_context;
    bool available = false;
    window_lock();
    for (uint32_t i = 0U; i < g_window_server.count; ++i) {
        window_server_window_t *window = g_window_server.windows[i];
        if (window->owner != context->process ||
            (context->identifier != 0U && window->identifier != context->identifier) ||
            window->event_count == 0U) continue;
        *context->event = window->events[window->event_read];
        window->event_read = (window->event_read + 1U) % WINDOW_EVENT_CAPACITY;
        --window->event_count;
        available = true;
        break;
    }
    window_unlock();
    return available;
}

kstatus_t window_server_event_read(process_t *process, uint32_t identifier,
                                   os_window_event_t *event, uint64_t timeout_ns) {
    window_event_wait_context_t context;
    window_server_window_t *wait_window = 0;
    wait_queue_t *wait_queue = &g_window_server.event_waitq;
    kstatus_t status;
    if (process == 0 || event == 0 || !window_server_init()) return K_EINVAL;
    context.process = process;
    context.identifier = identifier;
    context.event = event;
    if (window_event_available(&context)) return K_OK;
    if (timeout_ns == 0U) return K_EAGAIN;
    if (identifier != 0U) {
        /* Keep the embedded queue alive across a blocked read.  Teardown
         * wakes this queue before dropping the registry reference. */
        if (window_server_lookup(identifier, &wait_window) != K_OK ||
            wait_window->owner != process) {
            if (wait_window != 0) window_server_put(wait_window);
            return K_EPERM;
        }
        wait_queue = &wait_window->event_waitq;
    }
    status = wait_on_queue(wait_queue, window_event_available,
                           &context, timeout_ns);
    if (wait_window != 0) window_server_put(wait_window);
    return status;
}

kstatus_t window_server_update(process_t *process, uint32_t identifier,
                               int32_t x, int32_t y, uint32_t width,
                               uint32_t height, uint32_t flags) {
    window_server_window_t *window;
    bool report_surface = false;
    uint64_t first_present_tsc = 0U;
    uint64_t first_present_faults = 0U;
    uint64_t populated_pages = 0U;
    uint64_t prefaulted_pages = 0U;
    uint64_t fault_around_pages = 0U;
    uint64_t create_tsc = 0U;
    uint64_t create_begin_faults = 0U;
    if (process == 0 || identifier == 0U || flags != 0U ||
        (width == 0U) != (height == 0U)) return K_EINVAL;
    if (!window_server_kernel_ready()) return K_EIO;
    window_lock();
    window = window_scene_find_locked(identifier);
    if (window == 0 || window->owner != process) {
        window_unlock();
        return K_EPERM;
    }

    if (width != 0U && !window->surface_metrics_reported &&
        window->section != 0 && window->section->vm_object != 0) {
        window->surface_metrics_reported = true;
        report_surface = true;
        first_present_tsc = telemetry_timestamp();
        create_tsc = window->create_tsc;
        create_begin_faults = window->surface_fault_count_at_create;
        first_present_faults = vm_object_fault_count(
            window->section->vm_object);
        populated_pages = vm_object_populated_pages(
            window->section->vm_object);
        prefaulted_pages = vm_object_prefaulted_pages(
            window->section->vm_object);
        fault_around_pages = vm_object_fault_around_pages(
            window->section->vm_object);
    }

    /*
     * A resizable client acknowledges the current row stride by submitting a
     * full-surface damage rectangle with exactly the current width/height.
     * A stale redraw for an older resize cannot expose mis-strided pixels.
     */
    if (window->resize_pending && x == 0 && y == 0 &&
        width == window->width && height == window->height) {
        window->resize_pending = false;
    }

    window->dirty = true;
    if (width == 0U) {
        window_mark_window_locked(window);
    } else {
        window_mark_surface_locked(window, x, y, width, height);
    }
    window_unlock();
    if (report_surface) {
        kernel_perf_emit_value(
            "window_create_begin_fault_count",
            create_begin_faults);
        kernel_perf_emit_value(
            "window_first_present_fault_count", first_present_faults);
        kernel_perf_emit_value(
            "window_surface_faults",
            first_present_faults - create_begin_faults);
        kernel_perf_emit_value("window_pages_allocated", populated_pages);
        kernel_perf_emit_value("window_pages_prefaulted", prefaulted_pages);
        kernel_perf_emit_value("window_pages_fault_around", fault_around_pages);
        kernel_perf_emit_value("fault_count",
                               first_present_faults - create_begin_faults);
        kernel_perf_emit_value("shared_fault_count",
                               first_present_faults - create_begin_faults);
        kernel_perf_emit_value("pages_allocated", populated_pages);
        kernel_perf_emit_value("pages_prefaulted", prefaulted_pages);
        kernel_perf_emit_value("pages_fault_around", fault_around_pages);
        kernel_perf_emit_value(
            "window_first_present_latency_tsc",
            first_present_tsc >= create_tsc ?
                first_present_tsc - create_tsc : 0U);
        kernel_perf_emit_value(
            "first_present_us",
            first_present_tsc >= create_tsc ?
                window_tsc_to_us(first_present_tsc - create_tsc) : 0U);
        vm_fault_telemetry_report();
    }
    /* A client update is independent of input.  Wake the compositor even
     * when the input queue is empty; otherwise the worker can sleep until the
     * next mouse report and every redraw appears one event late. */
    window_server_notify_worker();
    return K_OK;
}
