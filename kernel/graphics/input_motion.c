#include "internal.h"

/* REFACTOR_P7A_INPUT_MOTION_OWNER: batched pointer conversion and flush policy. */

/* Pointer coordinates are input state, not window or compositor state. Keep
 * their mutation in this Owner so ordinary motion and drag batching share
 * one publication boundary. */
void window_input_set_pointer_locked(uint32_t x, uint32_t y) {
    g_window_server.pointer_x = x;
    g_window_server.pointer_y = y;
}

/*
 * Feed an accumulated relative axis back through the normal window routing
 * path. int64_t accumulation prevents a burst of raw events from overflowing
 * before it is emitted as one or more ABI-sized int32_t events.
 */
void window_route_motion_axis_locked(
    const window_motion_batch_t *batch,
    uint32_t code,
    int64_t delta) {
    input_event_t event = {0};

    if (batch == 0 || delta == 0) return;

    event.timestamp = batch->timestamp;
    event.device_id = batch->device_id;
    event.type = INPUT_EVENT_RELATIVE;
    event.flags = batch->flags;
    event.code = code;

    while (delta != 0) {
        int64_t chunk;

        if (delta > 2147483647LL) {
            chunk = 2147483647LL;
        } else if (delta < -2147483648LL) {
            chunk = -2147483648LL;
        } else {
            chunk = delta;
        }

        event.value = (int32_t)chunk;
        route_input_locked(&event);
        delta -= chunk;
    }
}

void window_flush_motion_batch_locked(window_motion_batch_t *batch) {
    if (batch == 0 || !batch->active) return;

    /* Decoration movement is consumed by Ring0, so X/Y may safely be applied
     * as one geometry update. */
    if (window_route_drag_motion_batch_locked(batch)) {
        batch->active = false;
        batch->delta_x = 0;
        batch->delta_y = 0;
        return;
    }

    /* Keep ordinary client-facing relative input exactly as before. */
    window_route_motion_axis_locked(batch, INPUT_REL_X, batch->delta_x);
    window_route_motion_axis_locked(batch, INPUT_REL_Y, batch->delta_y);
    batch->active = false;
    batch->delta_x = 0;
    batch->delta_y = 0;
}

/*
 * Consume one kernel-private HID pointer transaction. Ring3 still receives
 * the unchanged button/relative event ABI, while the compositor applies the
 * report's X/Y pair in one geometry transition.
 */
void window_route_pointer_transaction_locked(const input_event_t *event) {
    static const uint32_t button_codes[] = {
        INPUT_BUTTON_LEFT,
        INPUT_BUTTON_RIGHT,
        INPUT_BUTTON_MIDDLE,
    };
    window_motion_batch_t motion = {0};

    if (event == 0 || event->type != INPUT_EVENT_POINTER) return;

    for (uint32_t bit = 0U; bit < 3U; ++bit) {
        uint16_t mask = (uint16_t)(1U << bit);
        if ((event->code & mask) == 0U) continue;
        input_event_t button = {
            .timestamp = event->timestamp,
            .device_id = event->device_id,
            .type = INPUT_EVENT_BUTTON,
            .flags = event->flags,
            .code = button_codes[bit],
            .value = (event->value & mask) != 0U ?
                     INPUT_VALUE_PRESS : INPUT_VALUE_RELEASE,
        };
        route_input_locked(&button);
    }

    if (event->value2 != 0 || event->value3 != 0) {
        motion.active = true;
        motion.device_id = event->device_id;
        motion.flags = event->flags;
        motion.timestamp = event->timestamp;
        motion.delta_x = event->value2;
        motion.delta_y = event->value3;
        if (!window_route_drag_motion_batch_locked(&motion)) {
            window_route_motion_axis_locked(&motion, INPUT_REL_X,
                                            motion.delta_x);
            window_route_motion_axis_locked(&motion, INPUT_REL_Y,
                                            motion.delta_y);
        }
    }

    if (event->value4 != 0) {
        input_event_t wheel = {
            .timestamp = event->timestamp,
            .device_id = event->device_id,
            .type = INPUT_EVENT_RELATIVE,
            .flags = event->flags,
            .code = INPUT_REL_WHEEL,
            .value = event->value4,
        };
        route_input_locked(&wheel);
    }
    window_coalesce_damage_locked();
}
