#include "internal.h"

/* REFACTOR_P7A_WINDOW_EVENT_OWNER: queue storage, event translation, and
 * post-lock wake publication. */

static int32_t window_pointer_clamp_i32(
    int64_t value) {

    if (value < -2147483648LL) {
        return (-2147483647 - 1);
    }

    if (value > 2147483647LL) {
        return 2147483647;
    }

    return (int32_t)value;
}


/*
 * Convert the global Ring0 pointer position into the coordinate system used
 * by the application's client surface.
 *
 * The titlebar and outer border are not part of Ring3 coordinates.
 */
static void window_event_set_pointer_locked(
    const window_server_window_t *window,
    os_window_event_t *event) {

    int64_t local_x;
    int64_t local_y;

    if (window == 0 ||
        event == 0) {
        return;
    }

    local_x =
        (int64_t)g_window_server.pointer_x -
        (int64_t)window->x -
        (int64_t)window_client_offset_x(window->flags);

    local_y =
        (int64_t)g_window_server.pointer_y -
        (int64_t)window->y -
        (int64_t)window_client_offset_y(window->flags);

    event->pointer_x =
        window_pointer_clamp_i32(
            local_x);

    event->pointer_y =
        window_pointer_clamp_i32(
            local_y);
}

void window_event_reset_ready_locked(void) {
    for (uint32_t index = 0U;
         index < WINDOW_SERVER_MAX_WINDOWS;
         ++index) {
        g_window_server.event_ready_windows[index] = 0;
    }

    g_window_server.event_ready_count = 0U;
    g_window_server.event_ready_overflow = false;
}


/*
 * Record one window for a post-lock waiter wake.
 *
 * Normal path is O(1). A temporary object reference keeps the embedded wait
 * queue alive across concurrent close/process teardown.
 */
void window_event_schedule_wake_locked(
    window_server_window_t *window) {

    if (window == 0 ||
        window->event_wake_pending) {
        return;
    }

    if (g_window_server.event_ready_count >=
        WINDOW_SERVER_MAX_WINDOWS) {

        g_window_server.event_ready_overflow =
            true;

        return;
    }

    object_get(window);

    window->event_wake_pending = true;

    g_window_server.event_ready_windows[
        g_window_server.event_ready_count++] =
        window;
}


/*
 * Detach the ready set under window_lock, then perform wake_one/object_put
 * outside the lock.
 *
 * Only the extremely rare overflow path scans the registry.
 */
static void window_event_publish_wake(window_server_window_t *window) {
    if (window == 0) return;
    (void)wake_one(&window->event_waitq);
    object_notify_signaled(window);
    object_put(window);
}

bool window_flush_event_wakes(void) {
    window_server_window_t *ready[WINDOW_SERVER_MAX_WINDOWS];
    window_server_window_t *overflow_ready[WINDOW_SERVER_MAX_WINDOWS];
    uint32_t overflow_count = 0U;

    window_lock();
    uint32_t ready_count = g_window_server.event_ready_count;
    if (ready_count > WINDOW_SERVER_MAX_WINDOWS)
        ready_count = WINDOW_SERVER_MAX_WINDOWS;
    bool overflow = g_window_server.event_ready_overflow;

    for (uint32_t index = 0U; index < ready_count; ++index) {
        window_server_window_t *window = g_window_server.event_ready_windows[index];
        ready[index] = window;
        g_window_server.event_ready_windows[index] = 0;
        // Normal batches detach and clear membership in one pass. Overflow
        // batches retain membership until registry deduplication is complete.
        if (!overflow && window != 0) window->event_wake_pending = false;
    }
    g_window_server.event_ready_count = 0U;
    g_window_server.event_ready_overflow = false;

    if (overflow) {
        for (uint32_t index = 0U;
             index < g_window_server.count &&
             overflow_count < WINDOW_SERVER_MAX_WINDOWS; ++index) {
            window_server_window_t *window = g_window_server.windows[index];
            if (window == 0 || window->event_wake_pending ||
                window->event_count == 0U) continue;
            object_get(window);
            overflow_ready[overflow_count++] = window;
        }
        // Clear membership before unlocking so a concurrent enqueue can
        // schedule another wake during post-lock publication.
        for (uint32_t index = 0U; index < ready_count; ++index) {
            if (ready[index] != 0) ready[index]->event_wake_pending = false;
        }
    }
    window_unlock();

    for (uint32_t index = 0U; index < ready_count; ++index)
        window_event_publish_wake(ready[index]);
    for (uint32_t index = 0U; index < overflow_count; ++index)
        window_event_publish_wake(overflow_ready[index]);
    return ready_count != 0U || overflow_count != 0U;
}


/* All producers share slot eviction, clearing and post-payload publication. */
static os_window_event_t *window_event_reserve_locked(window_server_window_t *window) {
    if (window->event_count >= WINDOW_EVENT_CAPACITY) {
        window->event_read = (window->event_read + 1U) % WINDOW_EVENT_CAPACITY;
        --window->event_count;
    }
    os_window_event_t *queued = &window->events[window->event_write];
    *queued = (os_window_event_t){0};
    queued->identifier = window->identifier;
    return queued;
}

static void window_event_commit_locked(window_server_window_t *window) {
    window->event_write = (window->event_write + 1U) % WINDOW_EVENT_CAPACITY;
    ++window->event_count;
    window_event_schedule_wake_locked(window);
}

void window_enqueue_client_input_locked(window_server_window_t *window,
                                       const os_input_event_t *event) {
    if (window == 0 || event == 0) return;
    os_window_event_t *queued = window_event_reserve_locked(window);
    queued->type = OS_WINDOW_EVENT_INPUT;
    queued->input = *event;
    window_event_set_pointer_locked(window, queued);
    window_event_commit_locked(window);
}

void window_enqueue_event_locked(window_server_window_t *window,
                                 const input_event_t *event) {
    if (window == 0 || event == 0) return;
    os_window_event_t *queued = window_event_reserve_locked(window);
    queued->type = OS_WINDOW_EVENT_INPUT;
    queued->input.timestamp = event->timestamp;
    queued->input.device_id = event->device_id;
    queued->input.type = event->type;
    queued->input.flags = event->flags;
    queued->input.code = event->code;
    queued->input.value = event->value;
    window_event_set_pointer_locked(window, queued);
    window_event_commit_locked(window);
}

/* Close requests are cooperative and deduplicated until consumed or evicted.
 * Ring3, not this queue, owns normal process shutdown policy. */
void window_enqueue_close_request_locked(window_server_window_t *window) {
    if (window == 0) return;
    for (uint32_t offset = 0U; offset < window->event_count; ++offset) {
        uint32_t slot = (window->event_read + offset) % WINDOW_EVENT_CAPACITY;
        const os_window_event_t *queued = &window->events[slot];
        if (queued->identifier == window->identifier &&
            queued->type == OS_WINDOW_EVENT_CLOSE_REQUEST) return;
    }
    os_window_event_t *queued = window_event_reserve_locked(window);
    queued->type = OS_WINDOW_EVENT_CLOSE_REQUEST;
    window_event_commit_locked(window);
}

void window_enqueue_minimized_event_locked(window_server_window_t *window) {
    if (window == 0) return;
    os_window_event_t *queued = window_event_reserve_locked(window);
    queued->type = OS_WINDOW_EVENT_MINIMIZED;
    queued->minimized.minimized = window->minimized ? 1U : 0U;
    window_event_commit_locked(window);
}

void window_enqueue_resize_event_locked(window_server_window_t *window) {
    if (window == 0) return;
    /* Consecutive resize events collapse to the newest geometry without
     * crossing an intervening input, close or minimized event. */
    if (window->event_count != 0U) {
        uint32_t slot = (window->event_write + WINDOW_EVENT_CAPACITY - 1U) %
                        WINDOW_EVENT_CAPACITY;
        os_window_event_t *queued = &window->events[slot];
        if (queued->identifier == window->identifier &&
            queued->type == OS_WINDOW_EVENT_RESIZE) {
            queued->resize.width = window->width;
            queued->resize.height = window->height;
            queued->resize.buffer_size = window->buffer_size;
            queued->resize.reserved = 0U;
            return;
        }
    }
    os_window_event_t *queued = window_event_reserve_locked(window);
    queued->type = OS_WINDOW_EVENT_RESIZE;
    queued->resize.width = window->width;
    queued->resize.height = window->height;
    queued->resize.buffer_size = window->buffer_size;
    window_event_commit_locked(window);
}

void window_enqueue_moved_event_locked(window_server_window_t *window) {
    if (window == 0) return;
    os_window_event_t *queued = 0;
    /* Never coalesce across input: its client coordinates belong to the
     * geometry at that point in the stream. */
    if (window->event_count != 0U) {
        uint32_t slot = (window->event_write + WINDOW_EVENT_CAPACITY - 1U) %
                        WINDOW_EVENT_CAPACITY;
        if (window->events[slot].identifier == window->identifier &&
            window->events[slot].type == OS_WINDOW_EVENT_MOVED) {
            queued = &window->events[slot];
        }
    }
    bool append = queued == 0;
    if (append) queued = window_event_reserve_locked(window);
    queued->type = OS_WINDOW_EVENT_MOVED;
    queued->moved.x = window_pointer_clamp_i32(
        (int64_t)window->x + window_client_offset_x(window->flags));
    queued->moved.y = window_pointer_clamp_i32(
        (int64_t)window->y + window_client_offset_y(window->flags));
    queued->moved.reserved = 0U;
    queued->moved.reserved2 = 0U;
    if (append) window_event_commit_locked(window);
}
