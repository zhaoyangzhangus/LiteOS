#include <kernel/console.h>
#include "internal.h"

/* REFACTOR_P7A_INPUT_OWNER: title capture cleanup and event pump boundary. */

#ifndef LITEOS_REALTEST
#define LITEOS_REALTEST 0
#endif

void window_clear_title_capture_locked(void) {
    g_window_server.title_pressed_identifier = 0U;
    g_window_server.title_pressed_button = WINDOW_TITLE_BUTTON_NONE;
    g_window_server.title_pressed_pointer_x = 0U;
    g_window_server.title_pressed_pointer_y = 0U;
    g_window_server.title_pressed_client = false;
    g_window_server.title_pressed_event = (input_event_t){0};
}

void window_input_reset_desktop_state_locked(void) {
    g_window_server.desktop_pending_launch = DESKTOP_APP_NONE;
    g_window_server.desktop_gui_mask = 0U;
    g_window_server.desktop_tab_consumed = false;
    g_window_server.desktop_focus_cycle_requested = false;
}

uint32_t window_input_take_desktop_launch_locked(void) {
    uint32_t app = g_window_server.desktop_pending_launch;
    g_window_server.desktop_pending_launch = DESKTOP_APP_NONE;
    return app;
}

bool window_input_take_focus_cycle_request_locked(void) {
    bool requested = g_window_server.desktop_focus_cycle_requested;
    g_window_server.desktop_focus_cycle_requested = false;
    return requested;
}

bool window_title_capture_moved_locked(void) {
    uint32_t dx;
    uint32_t dy;

    if (g_window_server.title_pressed_identifier == 0U) {
        return false;
    }

    dx = g_window_server.pointer_x >= g_window_server.title_pressed_pointer_x ?
         g_window_server.pointer_x - g_window_server.title_pressed_pointer_x :
         g_window_server.title_pressed_pointer_x - g_window_server.pointer_x;
    dy = g_window_server.pointer_y >= g_window_server.title_pressed_pointer_y ?
         g_window_server.pointer_y - g_window_server.title_pressed_pointer_y :
         g_window_server.title_pressed_pointer_y - g_window_server.pointer_y;

    return dx >= WINDOW_TITLE_DRAG_DISTANCE ||
           dy >= WINDOW_TITLE_DRAG_DISTANCE;
}

static window_server_window_t *window_at_locked(uint32_t x, uint32_t y) {
    return window_scene_hit_test_locked(x, y);
}

static window_server_window_t *keyboard_window_locked(void) {
    return window_scene_keyboard_locked();
}

static uint32_t desktop_number_shortcut_app(uint32_t key_code) {
    static const uint32_t apps[] = {
        DESKTOP_APP_FILES,
        DESKTOP_APP_TERMINAL,
        DESKTOP_APP_NOTES,
        DESKTOP_APP_NETWORK,
        DESKTOP_APP_TASKMGR,
    };
    if (key_code < 0x1EU || key_code >= 0x1EU +
            sizeof(apps) / sizeof(apps[0])) {
        return DESKTOP_APP_NONE;
    }
    return apps[key_code - 0x1EU];
}

#if LITEOS_REALTEST
/* Keep only the zoom-key proof needed by the automated input regression. */
static void input_route_diag_key(const input_event_t *event,
                                 const window_server_window_t *target,
                                 bool delivered) {
    if (event == 0 || target == 0 || !delivered ||
        event->type != INPUT_EVENT_KEY || event->value != INPUT_VALUE_PRESS ||
        event->code != 0x2EU) return;
    liteos_serial_printf_serial_only(
        "LITEOS_DIAG_INPUT_ROUTE code=%u value=%u focus=%u target=%u delivered=%u\r\n",
        event->code, (uint32_t)event->value,
        g_window_server.focused_identifier, target->identifier, 1U);
}
#else
static void input_route_diag_key(const input_event_t *event,
                                 const window_server_window_t *target,
                                 bool delivered) {
    (void)event;
    (void)target;
    (void)delivered;
}
#endif

static void focus_locked(window_server_window_t *window) {
    window_scene_focus_locked(window);
}

uint32_t window_title_button_at_locked(
    const window_server_window_t *window,
    uint32_t pointer_x,
    uint32_t pointer_y) {

    static const uint32_t buttons[] = {
        WINDOW_TITLE_BUTTON_CLOSE,
        WINDOW_TITLE_BUTTON_MAXIMIZE,
        WINDOW_TITLE_BUTTON_MINIMIZE,
    };

    if (window == 0 || window_client_decorations(window->flags)) {
        return WINDOW_TITLE_BUTTON_NONE;
    }

    for (uint32_t index = 0U;
         index < sizeof(buttons) / sizeof(buttons[0]);
         ++index) {

        int32_t x;
        int32_t y;

        if (!window_title_button_rect(window->x,
                                      window->y,
                                      window->width,
                                      buttons[index],
                                      &x,
                                      &y)) {
            continue;
        }

        if ((int64_t)pointer_x >= x &&
            (int64_t)pointer_y >= y &&
            (int64_t)pointer_x < (int64_t)x + WINDOW_TITLE_BUTTON_SIZE &&
            (int64_t)pointer_y < (int64_t)y + WINDOW_TITLE_BUTTON_SIZE) {
            return buttons[index];
        }
    }

    return WINDOW_TITLE_BUTTON_NONE;
}

/*
 * Toggle a resizable window between normal and maximized geometry.
 *
 * width/height are always CLIENT dimensions. Decoration dimensions are
 * subtracted from the available work area before producing the resize event.
 */

/*
 * Hide a window without destroying its object, surface or client geometry.
 */
static bool window_minimize_locked(
    window_server_window_t *window) {

    window_server_window_t *new_focused = 0;

    if (window == 0 ||
        window->minimized ||
        (window->flags & OS_WINDOW_VISIBLE) == 0U) {
        return false;
    }

    /*
     * Erase the currently visible geometry.
     */
    window_mark_window_locked(
        window);

    window->minimized = true;
    window->dirty = true;

    /*
     * Cancel all decoration capture owned by this window.
     */
    if (g_window_server.dragging_identifier ==
        window->identifier) {
        window_input_clear_drag_locked();
    }

    if (g_window_server.title_pressed_identifier ==
        window->identifier) {
        window_clear_title_capture_locked();
    }

    /*
     * Transfer focus to the topmost remaining normal window.
     */
    if (g_window_server.focused_identifier ==
        window->identifier) {

        window_scene_set_focus_identifier_locked(0U);

        for (uint32_t index = g_window_server.count;
             index != 0U;
             --index) {

            window_server_window_t *candidate =
                g_window_server.windows[index - 1U];

            if (candidate == 0 ||
                candidate == window ||
                candidate->minimized ||
                (candidate->flags &
                 OS_WINDOW_VISIBLE) == 0U) {
                continue;
            }

            new_focused = candidate;
            window_scene_set_focus_identifier_locked(candidate->identifier);

            break;
        }
    }

    if (new_focused != 0) {
        window_mark_window_locked(
            new_focused);
    }

    return true;
}


static bool window_toggle_maximize_locked(
    window_server_window_t *window) {

    int32_t new_x;
    int32_t new_y;

    uint32_t new_width;
    uint32_t new_height;

    uint32_t available_height;

    if (window == 0 ||
        window_client_decorations(window->flags) ||
        (window->flags &
         OS_WINDOW_RESIZABLE) == 0U ||
        g_window_server.display_width <=
            WINDOW_FRAME_EXTRA ||
        g_window_server.display_height <=
            DESKTOP_TOPBAR_HEIGHT +
            WINDOW_TITLEBAR_HEIGHT +
            WINDOW_FRAME_EXTRA) {

        return false;
    }

    /*
     * Damage old geometry before mutating it.
     */
    window_mark_window_locked(
        window);

    if (!window->maximized) {
        /*
         * Preserve the exact normal client geometry.
         */
        window->restore_x =
            window->x;

        window->restore_y =
            window->y;

        window->restore_width =
            window->width;

        window->restore_height =
            window->height;

        new_x = 0;
        new_y =
            (int32_t)
                DESKTOP_TOPBAR_HEIGHT;

        new_width =
            g_window_server.display_width -
            WINDOW_FRAME_EXTRA;

        available_height =
            g_window_server.display_height -
            DESKTOP_TOPBAR_HEIGHT;

        new_height =
            available_height -
            WINDOW_TITLEBAR_HEIGHT -
            WINDOW_FRAME_EXTRA;

        window->maximized =
            true;

    } else {
        /*
         * Restore the exact geometry captured on entry to maximized state.
         */
        new_x =
            window->restore_x;

        new_y =
            window->restore_y;

        new_width =
            window->restore_width;

        new_height =
            window->restore_height;

        if (new_width == 0U ||
            new_height == 0U) {

            return false;
        }

        window->maximized =
            false;
    }

    window->x =
        new_x;

    window->y =
        new_y;

    window->width =
        new_width;

    window->height =
        new_height;

    window->dirty =
        true;

    /*
     * Until Ring3 acknowledges the new stride/geometry with a full-surface
     * update, compositor keeps the resize-safe path.
     */
    window->resize_pending =
        true;

    /*
     * Maximizing while a decoration capture exists must not leave any stale
     * drag/resize state behind.
     */
    if (g_window_server.dragging_identifier ==
        window->identifier) {
        window_input_clear_drag_locked();
    }

    window_mark_window_locked(
        window);

    window_enqueue_resize_event_locked(
        window);

    return true;
}
void route_input_locked(const input_event_t *event) {
    window_server_window_t *target = 0;
    bool deliver_input = true;
    uint32_t title_button =
        WINDOW_TITLE_BUTTON_NONE;

    if (event == 0) return;


    if (event->type == INPUT_EVENT_KEY) {
        window_server_window_t *keyboard_target = keyboard_window_locked();
        bool client_decorated =
            keyboard_target != 0 &&
            window_client_decorations(keyboard_target->flags);
        uint32_t shortcut_app = desktop_number_shortcut_app(event->code);

        /* A client-decorated window receives even modifier and switch-key
         * events.  Ring0's desktop shortcut belongs only to legacy windows. */
        if (!client_decorated &&
            (event->code == 0xE3U || event->code == 0xE7U)) {
            uint32_t bit = event->code == 0xE3U ? 1U : 2U;
            if (event->value == INPUT_VALUE_RELEASE) {
                g_window_server.desktop_gui_mask &= ~bit;
            } else {
                g_window_server.desktop_gui_mask |= bit;
            }
            deliver_input = false;
        } else if (!client_decorated &&
                   g_window_server.desktop_gui_mask != 0U &&
                   shortcut_app != DESKTOP_APP_NONE) {
            deliver_input = false;
            if (event->value == INPUT_VALUE_PRESS) {
                if (g_window_server.desktop_pending_launch ==
                    DESKTOP_APP_NONE) {
                    g_window_server.desktop_pending_launch = shortcut_app;
                }
                desktop_mark_app_locked(shortcut_app);
            }
        } else if (!client_decorated && event->code == 0x2BU) {
            if (g_window_server.desktop_gui_mask != 0U) {
                deliver_input = false;
                g_window_server.desktop_tab_consumed = true;
                if (event->value == INPUT_VALUE_PRESS) {
                    g_window_server.desktop_focus_cycle_requested = true;
                }
            } else if (g_window_server.desktop_tab_consumed) {
                deliver_input = false;
            }

            if (event->value == INPUT_VALUE_RELEASE) {
                g_window_server.desktop_tab_consumed = false;
            }
        }
    }

    if (event->type == INPUT_EVENT_RELATIVE) {
        uint32_t old_pointer_x = g_window_server.pointer_x;
        uint32_t old_pointer_y = g_window_server.pointer_y;
        int64_t next;

        if (event->code == INPUT_REL_X) {
            next = (int64_t)g_window_server.pointer_x + event->value;
            if (next < 0) next = 0;
            if (next >= g_window_server.display_width) {
                next = g_window_server.display_width == 0U ? 0 :
                       (int64_t)g_window_server.display_width - 1;
            }
            window_input_set_pointer_locked((uint32_t)next,
                                            g_window_server.pointer_y);
        } else if (event->code == INPUT_REL_Y) {
            next = (int64_t)g_window_server.pointer_y + event->value;
            if (next < 0) next = 0;
            if (next >= g_window_server.display_height) {
                next = g_window_server.display_height == 0U ? 0 :
                       (int64_t)g_window_server.display_height - 1;
            }
            window_input_set_pointer_locked(g_window_server.pointer_x,
                                            (uint32_t)next);
        }

        {
            uint32_t hovered = DESKTOP_APP_NONE;
            if (g_window_server.dragging_identifier == 0U &&
                window_at_locked(g_window_server.pointer_x,
                                 g_window_server.pointer_y) == 0) {
                hovered = desktop_app_at_locked(g_window_server.pointer_x,
                                                g_window_server.pointer_y);
            }
            if (hovered != g_window_server.desktop_hovered_app) {
                desktop_mark_app_locked(g_window_server.desktop_hovered_app);
                desktop_mark_app_locked(hovered);
                desktop_set_hovered_app_locked(hovered);
            }
        }

        /*
         * A title control is a click candidate until the pointer moves beyond
         * the drag slop.  This applies to both legacy compositor controls and
         * client-drawn header buttons; holding a still mouse never cancels a
         * click based on elapsed time.
         */
        if (g_window_server.dragging_identifier == 0U &&
            g_window_server.title_pressed_identifier != 0U) {
            window_server_window_t *pressed =
                window_scene_find_locked(
                    g_window_server.title_pressed_identifier);
            if (window_title_capture_moved_locked()) {
                if (pressed != 0 &&
                    (pressed->flags & OS_WINDOW_VISIBLE) != 0U &&
                    !pressed->maximized) {
                    window_input_begin_drag_locked(
                        pressed->identifier,
                        (int32_t)g_window_server.title_pressed_pointer_x -
                            pressed->x,
                        (int32_t)g_window_server.title_pressed_pointer_y -
                            pressed->y,
                        0U);
                    window_clear_title_capture_locked();
                } else {
                    window_clear_title_capture_locked();
                }
            }
        }

        if (g_window_server.dragging_identifier != 0U) {
            window_server_window_t *dragged =
                window_scene_find_locked(g_window_server.dragging_identifier);
            if (dragged == 0 || (dragged->flags & OS_WINDOW_VISIBLE) == 0U) {
                window_input_clear_drag_locked();
            } else if (g_window_server.resize_edges != 0U) {
                int32_t delta_x =
                    (int32_t)((int64_t)g_window_server.pointer_x - old_pointer_x);
                int32_t delta_y =
                    (int32_t)((int64_t)g_window_server.pointer_y - old_pointer_y);
                (void)window_resize_locked(dragged, delta_x, delta_y);
                target = dragged;
                /* Decoration capture: mouse deltas belong to Ring0, not client. */
                deliver_input = false;
            } else {
                int32_t old_x = dragged->x;
                int32_t old_y = dragged->y;
                int32_t new_x =
                    (int32_t)g_window_server.pointer_x -
                    g_window_server.drag_offset_x;
                int32_t new_y =
                    (int32_t)g_window_server.pointer_y -
                    g_window_server.drag_offset_y;
                uint32_t outer_width =
                    window_outer_width(dragged->width, dragged->flags);
                uint32_t outer_height =
                    window_outer_height(dragged->height, dragged->flags);

                bool reuse_retained =
                    g_window_server.drag_blit_valid &&
                    window_drag_reuse_safe_locked(
                        dragged,
                        old_x, old_y,
                        new_x, new_y,
                        outer_width, outer_height);

                window_input_record_drag_frame_locked(
                    reuse_retained,
                    old_x, old_y,
                    new_x, new_y,
                    outer_width, outer_height);

                dragged->x = new_x;
                dragged->y = new_y;

                window_mark_moved_rect_locked(
                    old_x, old_y, new_x, new_y,
                    outer_width, outer_height,
                    !reuse_retained);

                if (reuse_retained) {
                    window_mark_drag_corner_repair_locked(
                        new_x, new_y,
                        outer_width, outer_height,
                        dragged->flags);
                }

                dragged->dirty = true;
                target = dragged;
                deliver_input = false;
            }
        }

        if (target == 0) {
            target = window_at_locked(g_window_server.pointer_x,
                                      g_window_server.pointer_y);
        }
        window_mark_moved_cursor_locked(old_pointer_x, old_pointer_y,
                                        g_window_server.pointer_x,
                                        g_window_server.pointer_y);
    } else if (event->type == INPUT_EVENT_BUTTON) {
        target = window_at_locked(g_window_server.pointer_x,
                                  g_window_server.pointer_y);

        if (target != 0) {
            title_button =
                window_title_button_at_locked(
                    target,
                    g_window_server.pointer_x,
                    g_window_server.pointer_y);
        }

        /*
         * Button semantics are connected in the next step.
         *
         * Establish correct Ring0 ownership:
         *   - never deliver title-control clicks to Ring3
         *   - delay control activation until click-vs-drag is known
         *   - pressing a control still focuses its window
         */
        if (event->code ==
                INPUT_BUTTON_LEFT &&
            title_button !=
                WINDOW_TITLE_BUTTON_NONE) {

            deliver_input = false;

            if (event->value ==
                    INPUT_VALUE_PRESS &&
                target != 0) {

                /*
                 * Decoration capture begins here.
                 *
                 * Merely pressing a control focuses the window but performs
                 * no destructive action.
                 */
                focus_locked(target);

                g_window_server.title_pressed_identifier =
                    target->identifier;

                g_window_server.title_pressed_button =
                    title_button;
                g_window_server.title_pressed_pointer_x =
                    g_window_server.pointer_x;
                g_window_server.title_pressed_pointer_y =
                    g_window_server.pointer_y;
                g_window_server.title_pressed_client = false;
                g_window_server.title_pressed_event = *event;

            } else if (
                event->value ==
                    INPUT_VALUE_RELEASE) {

                /*
                 * Activate only if:
                 *
                 *   press window  == release window
                 *   press button  == release button
                 *
                 * Dragging away before release therefore cancels the action.
                 */
                if (target != 0 &&
                    g_window_server.title_pressed_identifier ==
                        target->identifier &&
                    g_window_server.title_pressed_button ==
                        title_button &&
                    !window_title_capture_moved_locked()) {

                    if (title_button ==
                        WINDOW_TITLE_BUTTON_CLOSE) {

                        window_enqueue_close_request_locked(
                            target);

                    } else if (
                        title_button ==
                        WINDOW_TITLE_BUTTON_MAXIMIZE) {

                        (void)window_toggle_maximize_locked(
                            target);

                    } else if (
                        title_button ==
                        WINDOW_TITLE_BUTTON_MINIMIZE) {

                        (void)window_minimize_locked(
                            target);
                    }
                }

                window_clear_title_capture_locked();
            }
        }


        if (event->code == INPUT_BUTTON_LEFT &&
            event->value == INPUT_VALUE_PRESS && target == 0) {
            uint32_t app = desktop_app_at_locked(g_window_server.pointer_x,
                                                 g_window_server.pointer_y);
            if (app != DESKTOP_APP_NONE) {
                if (g_window_server.desktop_pending_launch == DESKTOP_APP_NONE) {
                    g_window_server.desktop_pending_launch = app;
                }
                desktop_mark_app_locked(app);
                deliver_input = false;
            }
        }

        if (event->code ==
                INPUT_BUTTON_LEFT &&
            event->value ==
                INPUT_VALUE_PRESS &&
            title_button ==
                WINDOW_TITLE_BUTTON_NONE) {
            window_clear_title_capture_locked();
        }

        if (event->code == INPUT_BUTTON_LEFT &&
            event->value == INPUT_VALUE_PRESS &&
            target != 0 &&
            title_button == WINDOW_TITLE_BUTTON_NONE) {
            uint32_t resize_edges;
            focus_locked(target);
            resize_edges = window_resize_edges_locked(
                target, g_window_server.pointer_x, g_window_server.pointer_y);

            if (resize_edges != 0U) {
                window_input_begin_drag_locked(
                    target->identifier, 0, 0, resize_edges);
                deliver_input = false;
            } else if (
                window_client_decorations(target->flags) &&
                (int64_t)g_window_server.pointer_y - target->y >= 0 &&
                (int64_t)g_window_server.pointer_y - target->y <
                    WINDOW_CLIENT_DRAG_REGION_HEIGHT &&
                !window_client_drag_region(
                    target->flags, target->x, target->y, target->width,
                    g_window_server.pointer_x,
                    g_window_server.pointer_y)) {
                /* Client controls use the same press-vs-drag threshold as
                 * compositor controls.  The press is replayed on release
                 * only if the pointer never became a drag. */
                g_window_server.title_pressed_identifier =
                    target->identifier;
                g_window_server.title_pressed_button =
                    WINDOW_TITLE_BUTTON_NONE;
                g_window_server.title_pressed_pointer_x =
                    g_window_server.pointer_x;
                g_window_server.title_pressed_pointer_y =
                    g_window_server.pointer_y;
                g_window_server.title_pressed_client = true;
                g_window_server.title_pressed_event = *event;
                deliver_input = false;
            } else if (
                !target->maximized &&
                (window_client_drag_region(
                     target->flags, target->x, target->y, target->width,
                     g_window_server.pointer_x,
                     g_window_server.pointer_y) ||
                 (!window_client_decorations(target->flags) &&
                  (int64_t)g_window_server.pointer_y - target->y <
                  WINDOW_DRAG_REGION_HEIGHT))) {
                window_input_begin_drag_locked(
                    target->identifier,
                    (int32_t)g_window_server.pointer_x - target->x,
                    (int32_t)g_window_server.pointer_y - target->y,
                    0U);
                deliver_input = false;
            }
        } else if (event->code == INPUT_BUTTON_LEFT &&
                   event->value == INPUT_VALUE_RELEASE) {
            if (g_window_server.title_pressed_client) {
                window_server_window_t *pressed =
                    window_scene_find_locked(
                        g_window_server.title_pressed_identifier);
                input_event_t press_event =
                    g_window_server.title_pressed_event;

                if (pressed != 0 && target != 0 &&
                    pressed->identifier == target->identifier &&
                    !window_title_capture_moved_locked()) {
                    press_event.value = INPUT_VALUE_PRESS;
                    window_enqueue_event_locked(pressed, &press_event);
                    window_enqueue_event_locked(pressed, event);
                }
                window_clear_title_capture_locked();
                deliver_input = false;
            } else if (g_window_server.dragging_identifier != 0U) {
            target = window_scene_find_locked(g_window_server.dragging_identifier);
                deliver_input = false;
            }
            window_clear_title_capture_locked();
            window_input_clear_drag_locked();
        }
    } else if (event->type == INPUT_EVENT_KEY) {
        /*
         * Legacy windows keep the compositor's switch-key shortcut.  A
         * client-decorated surface owns its complete input contract, so the
         * shortcut must reach Ring3 unchanged instead of being consumed by
         * the window server.
         */
        window_server_window_t *keyboard_target = keyboard_window_locked();
        if (event->value != INPUT_VALUE_RELEASE && event->code == 0x2BU &&
            (keyboard_target == 0 ||
             !window_client_decorations(keyboard_target->flags))) {
            uint32_t position = 0U;
            if (g_window_server.count != 0U) {
                for (uint32_t index = 0U; index < g_window_server.count; ++index) {
                    window_server_window_t *candidate =
                        g_window_server.windows[index];
                    if (candidate != 0 && candidate->identifier ==
                        g_window_server.focused_identifier) {
                        position = index;
                        break;
                    }
                }
                for (uint32_t offset = 1U; offset <= g_window_server.count; ++offset) {
                    uint32_t index = (position + offset) % g_window_server.count;
                    window_server_window_t *candidate =
                        g_window_server.windows[index];
                    if (candidate != 0 &&
                        (candidate->flags & OS_WINDOW_VISIBLE) != 0U) {
                        focus_locked(candidate);
                        break;
                    }
                }
            }
        }
        target = keyboard_window_locked();
    }

    if (event->type == INPUT_EVENT_RELATIVE ||
        event->type == INPUT_EVENT_BUTTON) {
        window_coalesce_damage_locked();
    }
    input_route_diag_key(event, target, deliver_input && target != 0);
    if (deliver_input && target != 0) {
        window_enqueue_event_locked(target, event);
    }
}
