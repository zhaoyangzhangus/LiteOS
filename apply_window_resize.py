from pathlib import Path
import re

ROOT = Path(".")
buffers = {}

def get_text(path):
    if path not in buffers:
        p = ROOT / path
        if not p.is_file():
            raise SystemExit(f"{path}: file not found; run this from the LiteOS repository root")
        buffers[path] = p.read_text()
    return buffers[path]

def replace_once(path, old, new):
    data = get_text(path)
    if old not in data:
        raise SystemExit(f"{path}: expected source block not found; no files were written")
    buffers[path] = data.replace(old, new, 1)

def sub_once(path, pattern, replacement):
    data = get_text(path)
    data2, count = re.subn(pattern, lambda m: replacement, data, count=1,
                           flags=re.S)
    if count != 1:
        raise SystemExit(f"{path}: expected function block not found; no files were written")
    buffers[path] = data2

UAPI = "OS_Implementation_Specification_COMPLETE/include/uapi/window.h"

replace_once(
    UAPI,
'''enum os_window_flags {
    OS_WINDOW_VISIBLE = 1u << 0,
};
''',
'''enum os_window_flags {
    OS_WINDOW_VISIBLE   = 1u << 0,
    OS_WINDOW_RESIZABLE = 1u << 1,
};
''')

replace_once(
    UAPI,
'''typedef struct os_window_event {
    uint32_t identifier;
    uint32_t reserved;
    os_input_event_t input;
} os_window_event_t;
''',
'''enum os_window_event_type {
    /* INPUT remains zero so old zero-initialized event slots keep their meaning. */
    OS_WINDOW_EVENT_INPUT  = 0u,
    OS_WINDOW_EVENT_RESIZE = 1u,
};

typedef struct os_window_resize_event {
    uint32_t width;
    uint32_t height;
    uint64_t buffer_size;
    uint64_t reserved;
} os_window_resize_event_t;

typedef struct os_window_event {
    uint32_t identifier;
    uint32_t type;
    union {
        os_input_event_t input;
        os_window_resize_event_t resize;
    };
} os_window_event_t;
''')

replace_once(
    "kernel/core/syscall.c",
'''        (arguments.flags & ~OS_WINDOW_VISIBLE) != 0U ||
''',
'''        (arguments.flags & ~(OS_WINDOW_VISIBLE | OS_WINDOW_RESIZABLE)) != 0U ||
''')

replace_once(
    "include/kernel/window_server.h",
'''    uint64_t owner_address;
    bool dirty;
    char title[32];
''',
'''    uint64_t owner_address;
    bool dirty;
    bool resize_pending;
    char title[32];
''')

replace_once(
    "kernel/graphics/window_server.c",
'''#define WINDOW_CURSOR_HOTSPOT_X 3U
#define WINDOW_CURSOR_HOTSPOT_Y 1U
''',
'''#define WINDOW_CURSOR_HOTSPOT_X 3U
#define WINDOW_CURSOR_HOTSPOT_Y 1U
#define WINDOW_RESIZE_GRAB 6U
#define WINDOW_MIN_WIDTH 160U
#define WINDOW_MIN_HEIGHT 96U

enum {
    WINDOW_RESIZE_LEFT   = 1U << 0,
    WINDOW_RESIZE_RIGHT  = 1U << 1,
    WINDOW_RESIZE_TOP    = 1U << 2,
    WINDOW_RESIZE_BOTTOM = 1U << 3,
};
''')

replace_once(
    "kernel/graphics/window_server.c",
'''    uint32_t dragging_identifier;
    int32_t drag_offset_x;
    int32_t drag_offset_y;
    bool kernel_ready;
''',
'''    uint32_t dragging_identifier;
    int32_t drag_offset_x;
    int32_t drag_offset_y;
    uint32_t resize_edges;
    bool kernel_ready;
''')

replace_once(
    "kernel/graphics/window_server.c",
'''        g_window_server.dragging_identifier = 0U;
        g_window_server.drag_offset_x = 0;
        g_window_server.drag_offset_y = 0;
        g_window_server.kernel_ready = false;
''',
'''        g_window_server.dragging_identifier = 0U;
        g_window_server.drag_offset_x = 0;
        g_window_server.drag_offset_y = 0;
        g_window_server.resize_edges = 0U;
        g_window_server.kernel_ready = false;
''')

replace_once(
    "kernel/graphics/window_server.c",
'''        if (g_window_server.dragging_identifier == window->identifier) {
            g_window_server.dragging_identifier = 0U;
            g_window_server.drag_offset_x = 0;
            g_window_server.drag_offset_y = 0;
        }
''',
'''        if (g_window_server.dragging_identifier == window->identifier) {
            g_window_server.dragging_identifier = 0U;
            g_window_server.drag_offset_x = 0;
            g_window_server.drag_offset_y = 0;
            g_window_server.resize_edges = 0U;
        }
''')

create_fn = r'''kstatus_t window_server_create(process_t *owner, int32_t x, int32_t y,
                               uint32_t width, uint32_t height,
                               uint32_t flags, uint32_t background,
                               const char *title, window_server_window_t **out) {
    uint64_t pixels;
    uint64_t bytes;
    shared_section_t *section = 0;
    window_server_window_t *window;
    window_server_window_t *old_focused;

    if (out == 0 || owner == 0 || !window_server_init() || width == 0U ||
        height == 0U || width > UINT32_MAX / height ||
        (flags & ~(OS_WINDOW_VISIBLE | OS_WINDOW_RESIZABLE)) != 0U) {
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
    window = (window_server_window_t *)kzalloc(sizeof(*window), 0);
    if (window == 0) {
        object_put(section);
        return K_ENOMEM;
    }
    refcount_init(&window->object.refs, 1U);
    window->object.type = KOBJECT_TYPE_WINDOW;
    window->object.flags = 0U;
    window->object.ops = &g_window_object_ops;
    window->object.security = 0;
    window->owner = owner;
    object_get(owner);
    window->section = section;
    window->x = x;
    window->y = y;
    window->width = width;
    window->height = height;
    window->flags = flags;
    window->background = background;
    window->buffer_size = bytes;
    window->owner_address = 0U;
    window->dirty = true;
    window->resize_pending = false;
    window->event_read = 0U;
    window->event_write = 0U;
    window->event_count = 0U;
    for (uint32_t i = 0U; i + 1U < sizeof(window->title); ++i) {
        window->title[i] = title != 0 ? title[i] : '\0';
        if (window->title[i] == '\0') break;
    }
    window->title[sizeof(window->title) - 1U] = '\0';
    window_lock();
    if (g_window_server.count >= WINDOW_SERVER_MAX_WINDOWS) {
        window_unlock();
        object_put(window);
        return K_ENOMEM;
    }
    old_focused = find_window_locked(g_window_server.focused_identifier);
    window->identifier = g_window_server.next_identifier++;
    if (window->identifier == 0U) {
        window->identifier = g_window_server.next_identifier++;
    }
    g_window_server.windows[g_window_server.count++] = window;
    object_get(window); /* registry reference */
    if ((window->flags & OS_WINDOW_VISIBLE) != 0U) {
        g_window_server.focused_identifier = window->identifier;
    }
    window_mark_window_locked(old_focused);
    window_mark_window_locked(window);
    window_unlock();
    *out = window;
    return K_OK;
}

'''

sub_once(
    "kernel/graphics/window_server.c",
    r'''kstatus_t window_server_create\(process_t \*owner, int32_t x, int32_t y,.*?(?=kstatus_t window_server_lookup\()''',
    create_fn)

enqueue_fns = r'''static void window_enqueue_event_locked(window_server_window_t *window,
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
    window->event_write = (window->event_write + 1U) % WINDOW_EVENT_CAPACITY;
    ++window->event_count;
}

static void window_enqueue_resize_event_locked(window_server_window_t *window) {
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
}

'''

sub_once(
    "kernel/graphics/window_server.c",
    r'''static void window_enqueue_event_locked\(window_server_window_t \*window,.*?(?=kstatus_t window_server_register_manager\()''',
    enqueue_fns)

replace_once(
    "kernel/graphics/window_server.c",
'''    window->events[window->event_write].identifier = identifier;
    window->events[window->event_write].reserved = 0U;
    window->events[window->event_write].input = *event;
''',
'''    window->events[window->event_write].identifier = identifier;
    window->events[window->event_write].type = OS_WINDOW_EVENT_INPUT;
    window->events[window->event_write].input = *event;
''')

replace_once(
    "kernel/graphics/window_server.c",
'''        for (int64_t column = first_column; column < last_column; ++column) {
            uint64_t index = (uint64_t)row * window->width + column;
            uint64_t source = window->owner_address + index * sizeof(uint32_t);
            uint32_t pixel = window->background;
            if (window_source_pixel_locked(window, source, &pixel,
                                           &cached_page, &cached_base)) {
                *destination++ = pixel;
            } else {
                *destination++ = window->background;
            }
        }
''',
'''        for (int64_t column = first_column; column < last_column; ++column) {
            uint64_t index;
            uint64_t source;
            uint32_t pixel = window->background;

            if (window->resize_pending) {
                *destination++ = window->background;
                continue;
            }

            index = (uint64_t)row * window->width + column;
            source = window->owner_address + index * sizeof(uint32_t);
            if (window_source_pixel_locked(window, source, &pixel,
                                           &cached_page, &cached_base)) {
                *destination++ = pixel;
            } else {
                *destination++ = window->background;
            }
        }
''')

update_and_helpers = r'''kstatus_t window_server_update(process_t *process, uint32_t identifier,
                               int32_t x, int32_t y, uint32_t width,
                               uint32_t height, uint32_t flags) {
    window_server_window_t *window;
    if (process == 0 || identifier == 0U || flags != 0U ||
        (width == 0U) != (height == 0U)) return K_EINVAL;
    if (!window_server_kernel_ready()) return K_EIO;
    window_lock();
    window = find_window_locked(identifier);
    if (window == 0 || window->owner != process) {
        window_unlock();
        return K_EPERM;
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
    return K_OK;
}

static uint32_t window_resize_edges_locked(const window_server_window_t *window,
                                           uint32_t x, uint32_t y) {
    int64_t relative_x;
    int64_t relative_y;
    int64_t outer_width;
    int64_t outer_height;
    uint32_t edges = 0U;

    if (window == 0 || (window->flags & OS_WINDOW_RESIZABLE) == 0U) return 0U;

    relative_x = (int64_t)x - window->x;
    relative_y = (int64_t)y - window->y;
    outer_width = (int64_t)window->width + WINDOW_FRAME_EXTRA;
    outer_height = (int64_t)window->height + WINDOW_FRAME_EXTRA;
    if (relative_x < 0 || relative_y < 0 ||
        relative_x >= outer_width || relative_y >= outer_height) {
        return 0U;
    }

    if (relative_x < WINDOW_RESIZE_GRAB) {
        edges |= WINDOW_RESIZE_LEFT;
    } else if (relative_x >= outer_width - WINDOW_RESIZE_GRAB) {
        edges |= WINDOW_RESIZE_RIGHT;
    }

    if (relative_y < WINDOW_RESIZE_GRAB) {
        edges |= WINDOW_RESIZE_TOP;
    } else if (relative_y >= outer_height - WINDOW_RESIZE_GRAB) {
        edges |= WINDOW_RESIZE_BOTTOM;
    }
    return edges;
}

static bool window_resize_locked(window_server_window_t *window,
                                 int32_t delta_x, int32_t delta_y) {
    int32_t old_x;
    int32_t old_y;
    uint32_t old_width;
    uint32_t old_height;
    uint32_t max_width;
    uint32_t max_height;
    uint32_t min_width;
    uint32_t min_height;
    int64_t left;
    int64_t top;
    int64_t right;
    int64_t bottom;
    int64_t minimum;
    int64_t maximum;
    uint32_t new_width;
    uint32_t new_height;

    if (window == 0 || g_window_server.resize_edges == 0U ||
        (window->flags & OS_WINDOW_RESIZABLE) == 0U) {
        return false;
    }

    max_width = g_window_server.display_width;
    max_height = g_window_server.display_height;
    if (max_width == 0U || max_height == 0U) return false;
    min_width = max_width < WINDOW_MIN_WIDTH ? max_width : WINDOW_MIN_WIDTH;
    min_height = max_height < WINDOW_MIN_HEIGHT ? max_height : WINDOW_MIN_HEIGHT;

    old_x = window->x;
    old_y = window->y;
    old_width = window->width;
    old_height = window->height;

    left = window->x;
    top = window->y;
    right = left + (int64_t)window->width + WINDOW_FRAME_EXTRA;
    bottom = top + (int64_t)window->height + WINDOW_FRAME_EXTRA;

    if ((g_window_server.resize_edges & WINDOW_RESIZE_LEFT) != 0U) {
        left += delta_x;
        minimum = right - (int64_t)max_width - WINDOW_FRAME_EXTRA;
        maximum = right - (int64_t)min_width - WINDOW_FRAME_EXTRA;
        if (left < minimum) left = minimum;
        if (left > maximum) left = maximum;
    } else if ((g_window_server.resize_edges & WINDOW_RESIZE_RIGHT) != 0U) {
        right += delta_x;
        minimum = left + (int64_t)min_width + WINDOW_FRAME_EXTRA;
        maximum = left + (int64_t)max_width + WINDOW_FRAME_EXTRA;
        if (right < minimum) right = minimum;
        if (right > maximum) right = maximum;
    }

    if ((g_window_server.resize_edges & WINDOW_RESIZE_TOP) != 0U) {
        top += delta_y;
        minimum = bottom - (int64_t)max_height - WINDOW_FRAME_EXTRA;
        maximum = bottom - (int64_t)min_height - WINDOW_FRAME_EXTRA;
        if (top < minimum) top = minimum;
        if (top > maximum) top = maximum;
    } else if ((g_window_server.resize_edges & WINDOW_RESIZE_BOTTOM) != 0U) {
        bottom += delta_y;
        minimum = top + (int64_t)min_height + WINDOW_FRAME_EXTRA;
        maximum = top + (int64_t)max_height + WINDOW_FRAME_EXTRA;
        if (bottom < minimum) bottom = minimum;
        if (bottom > maximum) bottom = maximum;
    }

    new_width = (uint32_t)(right - left - WINDOW_FRAME_EXTRA);
    new_height = (uint32_t)(bottom - top - WINDOW_FRAME_EXTRA);
    if (left == old_x && top == old_y &&
        new_width == old_width && new_height == old_height) {
        return false;
    }

    window_mark_rect_locked(old_x, old_y,
                            old_width + WINDOW_FRAME_EXTRA,
                            old_height + WINDOW_FRAME_EXTRA);
    window->x = (int32_t)left;
    window->y = (int32_t)top;
    window->width = new_width;
    window->height = new_height;
    window->dirty = true;
    window->resize_pending = true;
    window_mark_window_locked(window);
    window_enqueue_resize_event_locked(window);
    return true;
}

'''

sub_once(
    "kernel/graphics/window_server.c",
    r'''kstatus_t window_server_update\(process_t \*process, uint32_t identifier,.*?(?=static void route_input_locked\()''',
    update_and_helpers)

route_fn = r'''static void route_input_locked(const input_event_t *event) {
    window_server_window_t *target = 0;
    bool deliver_input = true;

    if (event == 0) return;

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
            g_window_server.pointer_x = (uint32_t)next;
        } else if (event->code == INPUT_REL_Y) {
            next = (int64_t)g_window_server.pointer_y + event->value;
            if (next < 0) next = 0;
            if (next >= g_window_server.display_height) {
                next = g_window_server.display_height == 0U ? 0 :
                       (int64_t)g_window_server.display_height - 1;
            }
            g_window_server.pointer_y = (uint32_t)next;
        }

        if (g_window_server.dragging_identifier != 0U) {
            window_server_window_t *dragged =
                find_window_locked(g_window_server.dragging_identifier);
            if (dragged == 0 || (dragged->flags & OS_WINDOW_VISIBLE) == 0U) {
                g_window_server.dragging_identifier = 0U;
                g_window_server.drag_offset_x = 0;
                g_window_server.drag_offset_y = 0;
                g_window_server.resize_edges = 0U;
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
                dragged->x = (int32_t)g_window_server.pointer_x -
                             g_window_server.drag_offset_x;
                dragged->y = (int32_t)g_window_server.pointer_y -
                             g_window_server.drag_offset_y;
                window_mark_moved_rect_locked(
                    old_x, old_y, dragged->x, dragged->y,
                    dragged->width + WINDOW_FRAME_EXTRA,
                    dragged->height + WINDOW_FRAME_EXTRA);
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

        if (event->code == INPUT_BUTTON_LEFT &&
            event->value == INPUT_VALUE_PRESS && target != 0) {
            uint32_t resize_edges;
            focus_locked(target);
            resize_edges = window_resize_edges_locked(
                target, g_window_server.pointer_x, g_window_server.pointer_y);

            if (resize_edges != 0U) {
                g_window_server.dragging_identifier = target->identifier;
                g_window_server.drag_offset_x = 0;
                g_window_server.drag_offset_y = 0;
                g_window_server.resize_edges = resize_edges;
                deliver_input = false;
            } else if ((int64_t)g_window_server.pointer_y - target->y <
                       WINDOW_DRAG_REGION_HEIGHT) {
                g_window_server.dragging_identifier = target->identifier;
                g_window_server.drag_offset_x =
                    (int32_t)g_window_server.pointer_x - target->x;
                g_window_server.drag_offset_y =
                    (int32_t)g_window_server.pointer_y - target->y;
                g_window_server.resize_edges = 0U;
                deliver_input = false;
            }
        } else if (event->code == INPUT_BUTTON_LEFT &&
                   event->value == INPUT_VALUE_RELEASE) {
            if (g_window_server.dragging_identifier != 0U) {
                target = find_window_locked(g_window_server.dragging_identifier);
                deliver_input = false;
            }
            g_window_server.dragging_identifier = 0U;
            g_window_server.drag_offset_x = 0;
            g_window_server.drag_offset_y = 0;
            g_window_server.resize_edges = 0U;
        }
    } else if (event->type == INPUT_EVENT_KEY) {
        if (event->value != INPUT_VALUE_RELEASE && event->code == 0x2BU) {
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
    if (deliver_input && target != 0) {
        window_enqueue_event_locked(target, event);
    }
}

'''

sub_once(
    "kernel/graphics/window_server.c",
    r'''static void route_input_locked\(const input_event_t \*event\) \{.*?(?=void window_server_pump_input\()''',
    route_fn)

# gshell
replace_once(
    "user/gshell/main.c",
'''    request.flags = OS_WINDOW_VISIBLE;
''',
'''    request.flags = OS_WINDOW_VISIBLE | OS_WINDOW_RESIZABLE;
''')
replace_once(
    "user/gshell/main.c",
'''    request.identifier = g_shell.identifier;
    /* 0,0,0,0 means the complete client surface. */
    (void)gshell_syscall_one(OS_SYS_WINDOW_UPDATE, (uint64_t)&request);
''',
'''    request.identifier = g_shell.identifier;
    request.width = g_shell.width;
    request.height = g_shell.height;
    (void)gshell_syscall_one(OS_SYS_WINDOW_UPDATE, (uint64_t)&request);
''')
replace_once(
    "user/gshell/main.c",
'''    if (event == 0 || input == 0 || input->type != OS_INPUT_EVENT_KEY) return false;
''',
'''    if (event == 0 || event->type != OS_WINDOW_EVENT_INPUT ||
        input == 0 || input->type != OS_INPUT_EVENT_KEY) return false;
''')
replace_once(
    "user/gshell/main.c",
'''__attribute__((noreturn)) void gshell_entry(void) {
''',
'''static bool handle_event(const os_window_event_t *event) {
    uint64_t pixels;

    if (event == 0) return false;
    if (event->type == OS_WINDOW_EVENT_RESIZE) {
        if (event->resize.width == 0U || event->resize.height == 0U) return false;
        pixels = (uint64_t)event->resize.width * event->resize.height;
        if (pixels > event->resize.buffer_size / sizeof(uint32_t)) return false;
        g_shell.width = event->resize.width;
        g_shell.height = event->resize.height;
        render_window();
        return true;
    }
    return handle_key(event);
}

__attribute__((noreturn)) void gshell_entry(void) {
''')
replace_once(
    "user/gshell/main.c",
'''        if (status == 0) {
            (void)handle_key(&request.event);
''',
'''        if (status == 0) {
            (void)handle_event(&request.event);
''')

# fileman
replace_once(
    "user/fileman/main.c",
'''    request.flags = OS_WINDOW_VISIBLE;
''',
'''    request.flags = OS_WINDOW_VISIBLE | OS_WINDOW_RESIZABLE;
''')
replace_once(
    "user/fileman/main.c",
'''    request.identifier = g_window.identifier;
    (void)fileman_syscall_one(OS_SYS_WINDOW_UPDATE, (uint64_t)&request);
''',
'''    request.identifier = g_window.identifier;
    request.width = g_window.width;
    request.height = g_window.height;
    (void)fileman_syscall_one(OS_SYS_WINDOW_UPDATE, (uint64_t)&request);
''')
replace_once(
    "user/fileman/main.c",
'''    uint32_t rows = g_window.height > 58U ? (g_window.height - 58U) / 12U : 1U;
    if (input == 0 || input->type != OS_INPUT_EVENT_KEY) return;
''',
'''    uint32_t rows = g_window.height > 64U ?
        (g_window.height - 64U) / FONT12X24_HEIGHT : 1U;
    if (event == 0 || event->type != OS_WINDOW_EVENT_INPUT ||
        input == 0 || input->type != OS_INPUT_EVENT_KEY) return;
''')
replace_once(
    "user/fileman/main.c",
'''int main(int argc, char **argv) {
''',
'''static void handle_event(const os_window_event_t *event) {
    uint64_t pixels;

    if (event == 0) return;
    if (event->type == OS_WINDOW_EVENT_RESIZE) {
        if (event->resize.width == 0U || event->resize.height == 0U) return;
        pixels = (uint64_t)event->resize.width * event->resize.height;
        if (pixels > event->resize.buffer_size / sizeof(uint32_t)) return;
        g_window.width = event->resize.width;
        g_window.height = event->resize.height;
        render();
        return;
    }
    handle_key(event);
}

int main(int argc, char **argv) {
''')
replace_once(
    "user/fileman/main.c",
'''        if (status == 0) handle_key(&request.event);
''',
'''        if (status == 0) handle_event(&request.event);
''')

# notepad
replace_once(
    "user/notepad/main.c",
'''    request.flags = OS_WINDOW_VISIBLE;
''',
'''    request.flags = OS_WINDOW_VISIBLE | OS_WINDOW_RESIZABLE;
''')
replace_once(
    "user/notepad/main.c",
'''    request.identifier = g_window.identifier;
    (void)notepad_syscall_one(OS_SYS_WINDOW_UPDATE, (uint64_t)&request);
''',
'''    request.identifier = g_window.identifier;
    request.width = g_window.width;
    request.height = g_window.height;
    (void)notepad_syscall_one(OS_SYS_WINDOW_UPDATE, (uint64_t)&request);
''')
replace_once(
    "user/notepad/main.c",
'''    uint32_t columns = g_window.width > 20U ? (g_window.width - 20U) / 6U : 1U;
''',
'''    uint32_t columns = g_window.width > 20U ?
        (g_window.width - 20U) / FONT12X24_WIDTH : 1U;
''')
replace_once(
    "user/notepad/main.c",
'''    uint32_t columns = g_window.width > 20U ? (g_window.width - 20U) / 6U : 1U;
    uint32_t visible_lines = g_window.height > 46U ?
        (g_window.height - 46U) / 9U : 1U;
''',
'''    uint32_t columns = g_window.width > 20U ?
        (g_window.width - 20U) / FONT12X24_WIDTH : 1U;
    uint32_t visible_lines = g_window.height > 64U ?
        (g_window.height - 64U) / FONT12X24_HEIGHT : 1U;
''')
replace_once(
    "user/notepad/main.c",
'''    uint32_t page = g_window.height > 46U ? (g_window.height - 46U) / 9U : 1U;
''',
'''    uint32_t page = g_window.height > 64U ?
        (g_window.height - 64U) / FONT12X24_HEIGHT : 1U;
''')
replace_once(
    "user/notepad/main.c",
'''    if (event == 0 || input == 0 || input->type != OS_INPUT_EVENT_KEY) return false;
''',
'''    if (event == 0 || event->type != OS_WINDOW_EVENT_INPUT ||
        input == 0 || input->type != OS_INPUT_EVENT_KEY) return false;
''')
old_handle_event = '''static bool handle_event(const os_window_event_t *event) {
    const os_input_event_t *input = event != 0 ? &event->input : 0;
    if (input != 0 && input->type == OS_INPUT_EVENT_RELATIVE &&
        input->code == OS_INPUT_REL_WHEEL && input->value != 0) {
        scroll_editor(input->value > 0 ? -3 : 3);
        render();
        return true;
    }
    return handle_key(event);
}
'''
new_handle_event = '''static bool handle_event(const os_window_event_t *event) {
    const os_input_event_t *input;
    uint64_t pixels;

    if (event == 0) return false;
    if (event->type == OS_WINDOW_EVENT_RESIZE) {
        if (event->resize.width == 0U || event->resize.height == 0U) return false;
        pixels = (uint64_t)event->resize.width * event->resize.height;
        if (pixels > event->resize.buffer_size / sizeof(uint32_t)) return false;
        g_window.width = event->resize.width;
        g_window.height = event->resize.height;
        g_follow_cursor = true;
        render();
        return true;
    }
    if (event->type != OS_WINDOW_EVENT_INPUT) return false;

    input = &event->input;
    if (input->type == OS_INPUT_EVENT_RELATIVE &&
        input->code == OS_INPUT_REL_WHEEL && input->value != 0) {
        scroll_editor(input->value > 0 ? -3 : 3);
        render();
        return true;
    }
    return handle_key(event);
}
'''
replace_once("user/notepad/main.c", old_handle_event, new_handle_event)

for path, data in buffers.items():
    (ROOT / path).write_text(data)

print(f"window resize changes applied to {len(buffers)} files")
