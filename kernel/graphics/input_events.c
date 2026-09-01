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
bool window_flush_event_wakes(void) {
    window_server_window_t *
        ready[WINDOW_SERVER_MAX_WINDOWS];

    window_server_window_t *
        overflow_ready[WINDOW_SERVER_MAX_WINDOWS];

    uint32_t ready_count = 0U;
    uint32_t overflow_count = 0U;
    bool overflow;

    window_lock();

    ready_count =
        g_window_server.event_ready_count;

    if (ready_count >
        WINDOW_SERVER_MAX_WINDOWS) {
        ready_count =
            WINDOW_SERVER_MAX_WINDOWS;
    }

    for (uint32_t index = 0U;
         index < ready_count;
         ++index) {

        window_server_window_t *window =
            g_window_server.event_ready_windows[index];

        ready[index] = window;

        g_window_server.event_ready_windows[index] = 0;

        if (window != 0) {
            window->event_wake_pending = false;
        }
    }

    g_window_server.event_ready_count = 0U;

    overflow =
        g_window_server.event_ready_overflow;

    g_window_server.event_ready_overflow =
        false;

    if (overflow) {
        for (uint32_t index = 0U;
             index < g_window_server.count &&
             overflow_count < WINDOW_SERVER_MAX_WINDOWS;
             ++index) {

            window_server_window_t *window =
                g_window_server.windows[index];

            if (window == 0 ||
                window->event_count == 0U) {
                continue;
            }

            object_get(window);

            overflow_ready[
                overflow_count++] =
                window;
        }
    }

    window_unlock();

    for (uint32_t index = 0U;
         index < ready_count;
         ++index) {

        if (ready[index] == 0) {
            continue;
        }

        (void)wake_one(
            &ready[index]->event_waitq);

        object_put(
            ready[index]);
    }

    for (uint32_t index = 0U;
         index < overflow_count;
         ++index) {

        (void)wake_one(
            &overflow_ready[index]->event_waitq);

        object_put(
            overflow_ready[index]);
    }

    return
        ready_count != 0U ||
        overflow_count != 0U;
}


void window_enqueue_event_locked(window_server_window_t *window,
                                 const input_event_t *event) {
    if (window == 0 || event == 0) return;
    if (window->event_count >= WINDOW_EVENT_CAPACITY) {
        window->event_read = (window->event_read + 1U) % WINDOW_EVENT_CAPACITY;
        --window->event_count;
    }
    window->events[window->event_write].identifier = window->identifier;
    window->events[window->event_write].type = OS_WINDOW_EVENT_INPUT;
    window->events[window->event_write].input.timestamp = event->timestamp;
    window->events[window->event_write].input.device_id = event->device_id;
    window->events[window->event_write].input.type = event->type;
    window->events[window->event_write].input.flags = event->flags;
    window->events[window->event_write].input.code = event->code;
    window->events[window->event_write].input.value = event->value;

    window_event_set_pointer_locked(
        window,
        &window->events[
            window->event_write]);

    window->event_write = (window->event_write + 1U) % WINDOW_EVENT_CAPACITY;
    ++window->event_count;
    window_event_schedule_wake_locked(window);
}


/*
 * Queue one cooperative window-close request.
 *
 * No handle is closed and no process is terminated here. Ring3 remains the
 * owner of normal shutdown policy.
 */
void window_enqueue_close_request_locked(
    window_server_window_t *window) {

    os_window_event_t *queued;

    if (window == 0) {
        return;
    }

    /*
     * A close request is level-like from the application's point of view.
     * Avoid filling the fixed event queue with repeated close clicks while
     * the first request is still pending.
     */
    for (uint32_t offset = 0U;
         offset < window->event_count;
         ++offset) {

        uint32_t slot =
            (window->event_read + offset) %
            WINDOW_EVENT_CAPACITY;

        queued =
            &window->events[slot];

        if (queued->identifier ==
                window->identifier &&
            queued->type ==
                OS_WINDOW_EVENT_CLOSE_REQUEST) {

            return;
        }
    }

    if (window->event_count >=
        WINDOW_EVENT_CAPACITY) {

        window->event_read =
            (window->event_read + 1U) %
            WINDOW_EVENT_CAPACITY;

        --window->event_count;
    }

    queued =
        &window->events[
            window->event_write];

    /*
     * Clear the complete slot so the unused union payload never exposes
     * stale bytes from a previous INPUT/RESIZE event.
     */
    *queued =
        (os_window_event_t){0};

    queued->identifier =
        window->identifier;

    queued->type =
        OS_WINDOW_EVENT_CLOSE_REQUEST;

    window->event_write =
        (window->event_write + 1U) %
        WINDOW_EVENT_CAPACITY;

    ++window->event_count;
    window_event_schedule_wake_locked(window);
}


void window_enqueue_resize_event_locked(window_server_window_t *window) {
    os_window_event_t *queued;
    uint32_t slot;

    if (window == 0) return;

    /*
     * During a live resize only the newest size matters.  If the previous
     * queued event is already RESIZE, overwrite it instead of filling the
     * per-window queue with every mouse delta.
     */
    if (window->event_count != 0U) {
        slot = (window->event_write + WINDOW_EVENT_CAPACITY - 1U) %
               WINDOW_EVENT_CAPACITY;
        queued = &window->events[slot];
        if (queued->identifier == window->identifier &&
            queued->type == OS_WINDOW_EVENT_RESIZE) {
            queued->resize.width = window->width;
            queued->resize.height = window->height;
            queued->resize.buffer_size = window->buffer_size;
            queued->resize.reserved = 0U;
            return;
        }
    }

    if (window->event_count >= WINDOW_EVENT_CAPACITY) {
        window->event_read = (window->event_read + 1U) % WINDOW_EVENT_CAPACITY;
        --window->event_count;
    }

    queued = &window->events[window->event_write];
    queued->identifier = window->identifier;
    queued->type = OS_WINDOW_EVENT_RESIZE;
    queued->resize.width = window->width;
    queued->resize.height = window->height;
    queued->resize.buffer_size = window->buffer_size;
    queued->resize.reserved = 0U;
    window->event_write = (window->event_write + 1U) % WINDOW_EVENT_CAPACITY;
    ++window->event_count;
    window_event_schedule_wake_locked(window);
}
