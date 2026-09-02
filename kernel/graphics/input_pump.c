#include "internal.h"

/* REFACTOR_P7K_INPUT_PUMP_OWNER: input consumption and frame-pump policy. */

static inline bool window_input_router_is_relative_motion(
    const input_event_t *event) {
    return event != 0 &&
           event->type == INPUT_EVENT_RELATIVE &&
           (event->code == INPUT_REL_X ||
            event->code == INPUT_REL_Y);
}

/* Validate the event classification used by the real input pump without
 * injecting synthetic state into the window server. */
bool window_input_router_self_test(void) {
    input_event_t relative_x = {0};
    input_event_t relative_y = {0};
    input_event_t key = {0};

    relative_x.type = INPUT_EVENT_RELATIVE;
    relative_x.code = INPUT_REL_X;
    relative_y.type = INPUT_EVENT_RELATIVE;
    relative_y.code = INPUT_REL_Y;
    key.type = INPUT_EVENT_KEY;

    return window_input_router_is_relative_motion(&relative_x) &&
           window_input_router_is_relative_motion(&relative_y) &&
           !window_input_router_is_relative_motion(&key) &&
           WINDOW_COMPOSITOR_DRAG_INPUT_EVENTS >= WINDOW_EVENT_CAPACITY;
}

void window_server_pump_input_mode(bool compose_now) {
    input_event_t event;
    window_motion_batch_t motion = {0};
    bool wake = false;
    uint32_t consumed = 0U;
    uint32_t input_budget = WINDOW_EVENT_CAPACITY;

    if (!window_server_kernel_ready()) return;

    /* A ring-0 drag gets a larger pure-motion burst in one local batch. */
    window_lock();
    if (g_window_server.dragging_identifier != 0U) {
        input_budget = WINDOW_COMPOSITOR_DRAG_INPUT_EVENTS;
    }
    window_unlock();

    /* Relative X/Y and pure pointer-motion reports are coalesced. Any
     * button, wheel, key, or absolute event remains an ordering barrier. */
    while (consumed < input_budget &&
           input_core_pop(&event) == K_OK) {
        bool is_motion =
            window_input_router_is_relative_motion(&event);

        ++consumed;
        wake = true;

        if (event.type == INPUT_EVENT_POINTER) {
            bool pure_pointer_motion =
                event.code == 0U &&
                event.value4 == 0;

            if (pure_pointer_motion) {
                if (event.value2 == 0 &&
                    event.value3 == 0) {
                    continue;
                }

                if (motion.active &&
                    (motion.device_id != event.device_id ||
                     motion.flags != event.flags)) {

                    window_lock();
                    window_flush_motion_batch_locked(&motion);
                    window_unlock();
                }

                if (!motion.active) {
                    motion.active = true;
                    motion.device_id = event.device_id;
                    motion.flags = event.flags;
                    motion.timestamp = event.timestamp;
                    motion.delta_x = 0;
                    motion.delta_y = 0;
                }

                motion.timestamp = event.timestamp;
                motion.delta_x += event.value2;
                motion.delta_y += event.value3;
                continue;
            }

            window_lock();
            window_flush_motion_batch_locked(&motion);
            window_route_pointer_transaction_locked(&event);

            if (g_window_server.dragging_identifier != 0U) {
                input_budget = WINDOW_COMPOSITOR_DRAG_INPUT_EVENTS;
            }

            window_unlock();
            continue;
        }

        if (is_motion) {
            if (motion.active &&
                (motion.device_id != event.device_id ||
                 motion.flags != event.flags)) {

                window_lock();
                window_flush_motion_batch_locked(&motion);
                window_unlock();
            }

            if (!motion.active) {
                motion.active = true;
                motion.device_id = event.device_id;
                motion.flags = event.flags;
                motion.timestamp = event.timestamp;
                motion.delta_x = 0;
                motion.delta_y = 0;
            }

            motion.timestamp = event.timestamp;

            if (event.code == INPUT_REL_X) {
                motion.delta_x += event.value;
            } else {
                motion.delta_y += event.value;
            }

            continue;
        }

        window_lock();
        window_flush_motion_batch_locked(&motion);
        route_input_locked(&event);

        if (g_window_server.dragging_identifier != 0U) {
            input_budget = WINDOW_COMPOSITOR_DRAG_INPUT_EVENTS;
        }

        window_unlock();
    }

    if (motion.active) {
        window_lock();
        window_flush_motion_batch_locked(&motion);
        window_unlock();
    }

    /* Resize events are already published; populate their newly visible
     * surface pages before waking the client or composing the next frame. */
    window_surface_populate_pending();

    if (wake &&
        window_flush_event_wakes()) {
        (void)wake_all(&g_window_server.event_waitq);
    }

    for (;;) {
        uint32_t app;
        bool cycle_focus;

        window_lock();

        app = window_input_take_desktop_launch_locked();
        cycle_focus = window_input_take_focus_cycle_request_locked();

        window_unlock();

        if (app == DESKTOP_APP_NONE && !cycle_focus) {
            break;
        }

        if (cycle_focus) {
            desktop_cycle_window_focus();
        }

        if (app != DESKTOP_APP_NONE) {
            if (!desktop_restore_minimized_app(app)) {
                (void)desktop_launch_program(app);
            }
        }
    }

    if (!compose_now) return;

    compositor_frame_run();
}

void window_server_pump_input(void) {
    window_server_pump_input_mode(true);
}
