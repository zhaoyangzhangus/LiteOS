#include <kernel/window_server.h>
#include <arch/x86_64/paging.h>
#include <ascii_font.h>
#include <kernel/elf_loader.h>
#include <kernel/vfs.h>
#include <kernel/display.h>
#include <kernel/input.h>
#include <kernel/kmem.h>
#include <kernel/sched.h>
#include <kernel/vm.h>

#define WINDOW_DAMAGE_MAX_RECTS 16U
#define WINDOW_FRAME_BORDER 1U
#define WINDOW_FRAME_EXTRA (WINDOW_FRAME_BORDER * 2U)

#define WINDOW_TITLEBAR_HEIGHT 30U

/*
 * Flat modern Ring0 window controls.
 *
 * Buttons live completely inside the titlebar and stay clear of the
 * WINDOW_RESIZE_GRAB edge.
 */
#define WINDOW_TITLE_BUTTON_SIZE          20U
#define WINDOW_TITLE_BUTTON_GAP            4U
#define WINDOW_TITLE_BUTTON_RIGHT_MARGIN   6U

#define WINDOW_TITLE_CONTROLS_WIDTH \
    (WINDOW_TITLE_BUTTON_RIGHT_MARGIN + \
     WINDOW_TITLE_BUTTON_SIZE * 3U + \
     WINDOW_TITLE_BUTTON_GAP * 2U)


#define WINDOW_CURSOR_WIDTH 24U
#define WINDOW_CURSOR_HEIGHT 24U
#define WINDOW_CURSOR_HOTSPOT_X 3U
#define WINDOW_CURSOR_HOTSPOT_Y 1U

/* LiteOS desktop shell: wallpaper, icons and launcher. */
#define DESKTOP_TOPBAR_HEIGHT        38U

#define DESKTOP_DOCK_HEIGHT          84U
#define DESKTOP_DOCK_BOTTOM          18U
#define DESKTOP_DOCK_PADDING_X       16U
#define DESKTOP_DOCK_ICON_GAP        8U

#define DESKTOP_ICON_CELL_WIDTH      88U
#define DESKTOP_ICON_CELL_HEIGHT     68U
#define DESKTOP_ICON_START_X         24
#define DESKTOP_ICON_START_Y         58
#define DESKTOP_ICON_GAP_Y           12U
#define DESKTOP_ICON_IMAGE_WIDTH     48U
#define DESKTOP_ICON_IMAGE_HEIGHT    48U
#define DESKTOP_PROGRAM_MAX_BYTES    (16ULL * 1024ULL * 1024ULL)

enum {
    DESKTOP_APP_NONE = 0U,
    DESKTOP_APP_FILES = 1U,
    DESKTOP_APP_TERMINAL = 2U,
    DESKTOP_APP_NOTES = 3U,
    DESKTOP_APP_NETWORK = 4U,
};
#define WINDOW_RESIZE_GRAB 6U
#define WINDOW_MIN_WIDTH 160U
#define WINDOW_MIN_HEIGHT 96U
#define WINDOW_CLIENT_DRAG_REGION_HEIGHT 56U
/* input timestamps are raw TSC ticks; at a 3GHz guest this is about 150ms. */
#define WINDOW_TITLE_DRAG_DELAY_TSC_TICKS 450000000ULL

enum {
    WINDOW_RESIZE_LEFT   = 1U << 0,
    WINDOW_RESIZE_RIGHT  = 1U << 1,
    WINDOW_RESIZE_TOP    = 1U << 2,
    WINDOW_RESIZE_BOTTOM = 1U << 3,
};

/*
 * Most legacy clients still use the compositor-owned frame.  A client that
 * opts into OS_WINDOW_CLIENT_DECORATIONS owns the entire visible rectangle:
 * no Ring0 frame/titlebar is painted and pointer coordinates are delivered
 * directly in client space.
 */
static bool window_client_decorations(uint32_t flags) {
    return (flags & OS_WINDOW_CLIENT_DECORATIONS) != 0U;
}

static uint32_t window_frame_border(uint32_t flags) {
    return window_client_decorations(flags) ? 0U : WINDOW_FRAME_BORDER;
}

static uint32_t window_frame_extra(uint32_t flags) {
    return window_frame_border(flags) * 2U;
}

static uint32_t window_titlebar_height(uint32_t flags) {
    return window_client_decorations(flags) ? 0U : WINDOW_TITLEBAR_HEIGHT;
}

static uint32_t window_client_offset_y(uint32_t flags) {
    return window_frame_border(flags) + window_titlebar_height(flags);
}

static uint32_t window_client_offset_x(uint32_t flags) {
    return window_frame_border(flags);
}

static uint32_t window_outer_width(uint32_t width, uint32_t flags) {
    return width + window_frame_extra(flags);
}

static uint32_t window_outer_height(uint32_t height, uint32_t flags) {
    return height + window_frame_extra(flags) + window_titlebar_height(flags);
}

static bool window_client_drag_region(const window_server_window_t *window,
                                      uint32_t pointer_x,
                                      uint32_t pointer_y) {
    int64_t relative_x;
    int64_t relative_y;

    if (window == 0 || !window_client_decorations(window->flags)) {
        return false;
    }

    relative_x = (int64_t)pointer_x - window->x;
    relative_y = (int64_t)pointer_y - window->y;
    if (relative_x < 0 || relative_y < 0 ||
        relative_x >= (int64_t)window->width ||
        relative_y >= WINDOW_CLIENT_DRAG_REGION_HEIGHT) {
        return false;
    }

    /* Keep the app's search/menu/path and close controls clickable.  The
     * Files title itself and the blank header area remain drag handles. */
    if ((relative_x >= 45 && relative_x < 155) ||
        (relative_x >= 300 &&
         relative_x < (int64_t)window->width - 150)) {
        return true;
    }

    return false;
}


enum {
    WINDOW_TITLE_BUTTON_NONE = 0U,
    WINDOW_TITLE_BUTTON_MINIMIZE = 1U,
    WINDOW_TITLE_BUTTON_MAXIMIZE = 2U,
    WINDOW_TITLE_BUTTON_CLOSE = 3U,
};


typedef struct window_damage_rect {
    uint32_t left;
    uint32_t top;
    uint32_t right;
    uint32_t bottom;
} window_damage_rect_t;

/*
 * Immutable per-frame window view.
 *
 * reference keeps the real window object alive while the compositor runs
 * without window_lock.  All geometry/state used for this frame is copied
 * below so resize/move/focus changes can proceed concurrently.
 */
typedef struct compositor_window_view {
    window_server_window_t *reference;
    shared_section_t *section;

    uint32_t identifier;

    int32_t x;
    int32_t y;

    uint32_t width;
    uint32_t height;
    uint32_t flags;
    uint32_t background;

    uint64_t buffer_size;
    uint64_t owner_address;

    char title[32];

    bool resize_pending;
    bool maximized;
} compositor_window_view_t;

typedef struct compositor_snapshot {
    compositor_window_view_t windows[WINDOW_SERVER_MAX_WINDOWS];

    uint32_t window_count;
    uint32_t focused_identifier;

    uint32_t pointer_x;
    uint32_t pointer_y;

    uint32_t desktop_hovered_app;

    /*
     * Non-zero means this frame was produced while a window was being
     * dragged/resized.  The compositor can still render old/new damage
     * independently, but scanout publication must behave as one move
     * transaction to avoid exposing half-updated window positions.
     */
    uint32_t dragging_identifier;

    uint32_t damage_count;
    window_damage_rect_t damage_rects[WINDOW_DAMAGE_MAX_RECTS];

    /*
     * Current region being rendered.  Only the single active compositor
     * writes these fields.
     */
    uint32_t damage_left;
    uint32_t damage_top;
    uint32_t damage_right;
    uint32_t damage_bottom;
} compositor_snapshot_t;

static compositor_snapshot_t g_compositor_snapshot;




typedef struct{
    uint32_t left;
    uint32_t top;
    uint32_t right;
    uint32_t bottom;
}RECT;



static struct {
    spinlock_t lock;
    wait_queue_t event_waitq;
    window_server_window_t *windows[WINDOW_SERVER_MAX_WINDOWS];
    uint32_t count;
    uint32_t next_identifier;
    uint32_t focused_identifier;
    process_t *manager;
    uint32_t display_width;
    uint32_t display_height;
    uint32_t display_stride;
    uint32_t display_format;
    volatile uint32_t *framebuffer;
    volatile uint32_t *composite_framebuffer;
    uint32_t pointer_x;
    uint32_t pointer_y;
    uint32_t desktop_hovered_app;
    uint32_t desktop_pending_launch;
    uint32_t desktop_gui_mask;
    bool desktop_tab_consumed;
    bool desktop_focus_cycle_requested;
    uint32_t dragging_identifier;
    int32_t drag_offset_x;
    int32_t drag_offset_y;
    uint32_t resize_edges;

    /*
     * Decoration-button capture.
     *
     * A control activates only if press and release occur on the same
     * window/button pair before the timestamp-based drag delay expires.
     */
    uint32_t title_pressed_identifier;
    uint32_t title_pressed_button;
    uint32_t title_pressed_pointer_x;
    uint32_t title_pressed_pointer_y;
    bool title_pressed_client;
    input_event_t title_pressed_event;

    bool kernel_ready;
    bool dirty;
    bool composing;
    bool damage_full;
    uint32_t damage_count;
    window_damage_rect_t damage_rects[WINDOW_DAMAGE_MAX_RECTS];
    /* 当前正在合成的矩形，绘制热路径通过它做裁剪。 */
    uint32_t damage_left;
    uint32_t damage_top;
    uint32_t damage_right;
    uint32_t damage_bottom;
} g_window_server;

static atomic_uint g_window_server_init_state;

static void window_clear_title_capture_locked(void) {
    g_window_server.title_pressed_identifier = 0U;
    g_window_server.title_pressed_button = WINDOW_TITLE_BUTTON_NONE;
    g_window_server.title_pressed_pointer_x = 0U;
    g_window_server.title_pressed_pointer_y = 0U;
    g_window_server.title_pressed_client = false;
    g_window_server.title_pressed_event = (input_event_t){0};
}

static bool window_title_capture_elapsed_locked(const input_event_t *event) {
    uint64_t pressed_timestamp;
    if (event == 0 || g_window_server.title_pressed_identifier == 0U) {
        return false;
    }
    pressed_timestamp = g_window_server.title_pressed_event.timestamp;
    if (pressed_timestamp == 0U || event->timestamp < pressed_timestamp) {
        return false;
    }
    return event->timestamp - pressed_timestamp >=
           WINDOW_TITLE_DRAG_DELAY_TSC_TICKS;
}

static void window_mark_dirty_locked(void);
static void window_mark_rect_locked(int32_t x, int32_t y,
                                    uint32_t width, uint32_t height);
static void window_mark_window_locked(const window_server_window_t *window);
static void window_mark_surface_locked(const window_server_window_t *window,
                                       int32_t x, int32_t y,
                                       uint32_t width, uint32_t height);
static void window_mark_moved_rect_locked(int32_t old_x, int32_t old_y,
                                          int32_t new_x, int32_t new_y,
                                          uint32_t width, uint32_t height);
static void window_mark_moved_cursor_locked(uint32_t old_x, uint32_t old_y,
                                            uint32_t new_x, uint32_t new_y);
static void window_coalesce_damage_locked(void);
static void focus_locked(window_server_window_t *window);

static void window_lock(void) {
    sched_preempt_disable();
    while (atomic_exchange_explicit(&g_window_server.lock.state, 1U,
                                    memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
}

static void window_unlock(void) {
    atomic_store_explicit(&g_window_server.lock.state, 0U, memory_order_release);
    sched_preempt_enable();
}

bool window_server_init(void) {
    unsigned expected = 0U;
    if (atomic_compare_exchange_strong_explicit(&g_window_server_init_state,
                                                &expected, 1U,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        atomic_init(&g_window_server.lock.state, 0U);
        wait_queue_init(&g_window_server.event_waitq);
        g_window_server.count = 0U;
        g_window_server.next_identifier = 1U;
        g_window_server.focused_identifier = 0U;
        g_window_server.manager = 0;
        g_window_server.display_width = 0U;
        g_window_server.display_height = 0U;
        g_window_server.display_stride = 0U;
        g_window_server.display_format = 0U;
        g_window_server.framebuffer = 0;
        g_window_server.composite_framebuffer = 0;
        g_window_server.pointer_x = 0U;
        g_window_server.pointer_y = 0U;
        g_window_server.desktop_hovered_app = DESKTOP_APP_NONE;
        g_window_server.desktop_pending_launch = DESKTOP_APP_NONE;
        g_window_server.desktop_gui_mask = 0U;
        g_window_server.desktop_tab_consumed = false;
        g_window_server.desktop_focus_cycle_requested = false;
        g_window_server.dragging_identifier = 0U;
        g_window_server.drag_offset_x = 0;
        g_window_server.drag_offset_y = 0;
        g_window_server.resize_edges = 0U;
        g_window_server.title_pressed_identifier = 0U;
        g_window_server.title_pressed_button =
            WINDOW_TITLE_BUTTON_NONE;
        g_window_server.title_pressed_pointer_x = 0U;
        g_window_server.title_pressed_pointer_y = 0U;
        g_window_server.title_pressed_client = false;
        g_window_server.title_pressed_event = (input_event_t){0};
        g_window_server.kernel_ready = false;
        g_window_server.dirty = false;
        g_window_server.composing = false;
        g_window_server.damage_full = false;
        g_window_server.damage_count = 0U;
        g_window_server.damage_left = 0U;
        g_window_server.damage_top = 0U;
        g_window_server.damage_right = 0U;
        g_window_server.damage_bottom = 0U;
        for (uint32_t i = 0U; i < WINDOW_SERVER_MAX_WINDOWS; ++i) {
            g_window_server.windows[i] = 0;
        }
        atomic_store_explicit(&g_window_server_init_state, 2U,
                              memory_order_release);
        return true;
    }
    while (atomic_load_explicit(&g_window_server_init_state,
                                memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
    return true;
}

bool window_server_kernel_ready(void) {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    if (!window_server_init()) return false;
    if (g_window_server.kernel_ready) return true;
    if (!display_core_query(0U, &width, &height, &stride, &format)) return false;
    g_window_server.display_width = width;
    g_window_server.display_height = height;
    g_window_server.display_stride = stride;
    g_window_server.display_format = format;
    g_window_server.framebuffer =
        (volatile uint32_t *)(uintptr_t)display_core_framebuffer_virtual();
    if (g_window_server.framebuffer == 0 || width == 0U || height == 0U ||
        stride < width) return false;
    /* 先在内核后备帧缓冲中完成整帧合成，再一次性提交到 GOP framebuffer。
     * 这样清屏、画窗口和画光标的中间状态不会直接暴露给显示设备。 */
    if (g_window_server.composite_framebuffer == 0) {
        uint64_t bytes = (uint64_t)stride * height * sizeof(uint32_t);
        g_window_server.composite_framebuffer =
            (volatile uint32_t *)kzalloc((size_t)bytes, 0);
        if (g_window_server.composite_framebuffer == 0) {
            /* 内存不足时保持原路径，窗口仍可显示。 */
            g_window_server.composite_framebuffer = g_window_server.framebuffer;
        }
    }
    g_window_server.pointer_x = width / 2U;
    g_window_server.pointer_y = height / 2U;
    g_window_server.kernel_ready = true;
    window_mark_dirty_locked();
    return true;
}

static void window_object_destroy(void *raw_object) {
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

static const object_ops_t g_window_object_ops = {
    .destroy = window_object_destroy,
    .type_name = "Window",
    .is_signaled = 0,
    .wait_value = 0,
};

static void remove_window_locked(window_server_window_t *window) {
    window_server_window_t *new_focused = 0;
    for (uint32_t i = 0U; i < g_window_server.count; ++i) {
        if (g_window_server.windows[i] != window) continue;
        window_mark_window_locked(window);
        for (uint32_t j = i + 1U; j < g_window_server.count; ++j) {
            g_window_server.windows[j - 1U] = g_window_server.windows[j];
        }
        --g_window_server.count;
        g_window_server.windows[g_window_server.count] = 0;
        if (g_window_server.focused_identifier == window->identifier) {
            g_window_server.focused_identifier = 0U;
            for (uint32_t j = g_window_server.count; j != 0U; --j) {
                window_server_window_t *candidate =
                    g_window_server.windows[j - 1U];
                if (!candidate->minimized &&
                    (candidate->flags & OS_WINDOW_VISIBLE) != 0U) {
                    g_window_server.focused_identifier = candidate->identifier;
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
            g_window_server.dragging_identifier = 0U;
            g_window_server.drag_offset_x = 0;
            g_window_server.drag_offset_y = 0;
            g_window_server.resize_edges = 0U;
        }
        window_mark_window_locked(new_focused);
        object_put(window); /* registry reference */
        return;
    }
}

static window_server_window_t *find_window_locked(uint32_t identifier) {
    if (identifier == 0U) return 0;
    for (uint32_t i = 0U; i < g_window_server.count; ++i) {
        if (g_window_server.windows[i] != 0 &&
            g_window_server.windows[i]->identifier == identifier) {
            return g_window_server.windows[i];
        }
    }
    return 0;
}

static void window_mark_dirty_locked(void) {
    g_window_server.dirty = true;
    g_window_server.damage_full = true;
    g_window_server.damage_count = 0U;
    g_window_server.damage_left = 0U;
    g_window_server.damage_top = 0U;
    g_window_server.damage_right = g_window_server.display_width;
    g_window_server.damage_bottom = g_window_server.display_height;
}

static void window_mark_rect_locked(int32_t x, int32_t y,
                                    uint32_t width, uint32_t height) {
    int64_t right = (int64_t)x + width;
    int64_t bottom = (int64_t)y + height;
    uint32_t left;
    uint32_t top;
    if (width == 0U || height == 0U || g_window_server.display_width == 0U ||
        g_window_server.display_height == 0U || right <= 0 || bottom <= 0 ||
        x >= (int32_t)g_window_server.display_width ||
        y >= (int32_t)g_window_server.display_height) return;
    left = x < 0 ? 0U : (uint32_t)x;
    top = y < 0 ? 0U : (uint32_t)y;
    if (right > (int64_t)g_window_server.display_width) {
        right = g_window_server.display_width;
    }
    if (bottom > (int64_t)g_window_server.display_height) {
        bottom = g_window_server.display_height;
    }
    if (!g_window_server.dirty) g_window_server.dirty = true;
    if (g_window_server.damage_full) return;

    window_damage_rect_t candidate = {
        .left = left,
        .top = top,
        .right = (uint32_t)right,
        .bottom = (uint32_t)bottom,
    };
    for (uint32_t index = 0U; index < g_window_server.damage_count; ++index) {
        window_damage_rect_t *current = &g_window_server.damage_rects[index];
        uint32_t merged_left;
        uint32_t merged_top;
        uint32_t merged_right;
        uint32_t merged_bottom;
        uint32_t overlap_left;
        uint32_t overlap_top;
        uint32_t overlap_right;
        uint32_t overlap_bottom;
        uint64_t current_area;
        uint64_t candidate_area;
        uint64_t overlap_area = 0U;
        uint64_t union_area;
        uint64_t merged_area;

        if (candidate.right < current->left ||
            current->right < candidate.left ||
            candidate.bottom < current->top ||
            current->bottom < candidate.top) {
            continue;
        }

        merged_left = candidate.left < current->left ?
                      candidate.left : current->left;
        merged_top = candidate.top < current->top ?
                     candidate.top : current->top;
        merged_right = candidate.right > current->right ?
                       candidate.right : current->right;
        merged_bottom = candidate.bottom > current->bottom ?
                        candidate.bottom : current->bottom;

        overlap_left = candidate.left > current->left ?
                       candidate.left : current->left;
        overlap_top = candidate.top > current->top ?
                      candidate.top : current->top;
        overlap_right = candidate.right < current->right ?
                        candidate.right : current->right;
        overlap_bottom = candidate.bottom < current->bottom ?
                         candidate.bottom : current->bottom;

        current_area =
            (uint64_t)(current->right - current->left) *
            (current->bottom - current->top);
        candidate_area =
            (uint64_t)(candidate.right - candidate.left) *
            (candidate.bottom - candidate.top);

        if (overlap_left < overlap_right &&
            overlap_top < overlap_bottom) {
            overlap_area =
                (uint64_t)(overlap_right - overlap_left) *
                (overlap_bottom - overlap_top);
        }

        union_area = current_area + candidate_area - overlap_area;

        merged_area =
            (uint64_t)(merged_right - merged_left) *
            (merged_bottom - merged_top);

        /*
         * Bounding-box merge is only worthwhile when it wastes at most 25%
         * compared with the real union.  This prevents a diagonal chain of
         * small mouse damages from gradually becoming one huge rectangle.
         */
        if (merged_area > union_area + union_area / 4U) {
            continue;
        }

        current->left = merged_left;
        current->top = merged_top;
        current->right = merged_right;
        current->bottom = merged_bottom;
        return;
    }
    if (g_window_server.damage_count >= WINDOW_DAMAGE_MAX_RECTS) {
        g_window_server.damage_full = true;
        g_window_server.damage_count = 0U;
        g_window_server.damage_left = 0U;
        g_window_server.damage_top = 0U;
        g_window_server.damage_right = g_window_server.display_width;
        g_window_server.damage_bottom = g_window_server.display_height;
        return;
    }
    g_window_server.damage_rects[g_window_server.damage_count++] = candidate;
}

static void window_mark_window_locked(const window_server_window_t *window) {
    if (window == 0) return;
    window_mark_rect_locked(window->x, window->y,
                            window_outer_width(window->width, window->flags),
                            window_outer_height(window->height, window->flags));
}

static void window_mark_surface_locked(const window_server_window_t *window,
                                       int32_t x, int32_t y,
                                       uint32_t width, uint32_t height) {
    int64_t left;
    int64_t top;
    int64_t right;
    int64_t bottom;
    if (window == 0 || width == 0U || height == 0U) return;
    left = (int64_t)window->x +
           window_client_offset_x(window->flags) + x;
    top =
        (int64_t)window->y +
        window_client_offset_y(window->flags) +
        y;
    right = left + width;
    bottom = top + height;
    if (right <= 0 || bottom <= 0 ||
        left >= (int64_t)g_window_server.display_width ||
        top >= (int64_t)g_window_server.display_height) return;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > (int64_t)g_window_server.display_width) {
        right = g_window_server.display_width;
    }
    if (bottom > (int64_t)g_window_server.display_height) {
        bottom = g_window_server.display_height;
    }
    if (left >= right || top >= bottom) return;
    window_mark_rect_locked((int32_t)left, (int32_t)top,
                            (uint32_t)(right - left),
                            (uint32_t)(bottom - top));
}

/*
 * Window movement produces two logical damages:
 *
 *   1. old position: restore whatever is now underneath the window
 *   2. new position: draw the window at its new coordinates
 *
 * Do not immediately convert them into one bounding rectangle.  The generic
 * damage merge code already combines nearby/overlapping rectangles when the
 * bounding box wastes <=25%, while large moves remain as two independent
 * regions.
 *
 * Framebuffer publication is performed atomically for the whole compositor
 * snapshot, so keeping two regions no longer exposes an intermediate frame.
 */
static void window_mark_moved_rect_locked(
    int32_t old_x,
    int32_t old_y,
    int32_t new_x,
    int32_t new_y,
    uint32_t width,
    uint32_t height) {

    int64_t old_left;
    int64_t old_top;
    int64_t old_right;
    int64_t old_bottom;

    int64_t new_left;
    int64_t new_top;
    int64_t new_right;
    int64_t new_bottom;

    int64_t overlap_left;
    int64_t overlap_top;
    int64_t overlap_right;
    int64_t overlap_bottom;

    if (width == 0U ||
        height == 0U) {
        return;
    }

    if (old_x == new_x &&
        old_y == new_y) {

        window_mark_rect_locked(
            old_x,
            old_y,
            width,
            height);

        return;
    }

    old_left = old_x;
    old_top = old_y;
    old_right =
        old_left + (int64_t)width;
    old_bottom =
        old_top + (int64_t)height;

    new_left = new_x;
    new_top = new_y;
    new_right =
        new_left + (int64_t)width;
    new_bottom =
        new_top + (int64_t)height;

    overlap_left =
        old_left > new_left ?
        old_left : new_left;

    overlap_top =
        old_top > new_top ?
        old_top : new_top;

    overlap_right =
        old_right < new_right ?
        old_right : new_right;

    overlap_bottom =
        old_bottom < new_bottom ?
        old_bottom : new_bottom;

    /*
     * No overlap: retain the normal old/new pair.
     */
    if (overlap_left >= overlap_right ||
        overlap_top >= overlap_bottom) {

        window_mark_rect_locked(
            old_x,
            old_y,
            width,
            height);

        window_mark_rect_locked(
            new_x,
            new_y,
            width,
            height);

        return;
    }

    /*
     * OLD \ NEW:
     *
     * horizontal exposed strip across full old height.
     */
    if (new_left > old_left) {
        uint32_t exposed_width =
            (uint32_t)(
                overlap_left -
                old_left);

        if (exposed_width != 0U) {
            window_mark_rect_locked(
                old_x,
                old_y,
                exposed_width,
                height);
        }
    } else if (new_left < old_left) {
        uint32_t exposed_width =
            (uint32_t)(
                old_right -
                overlap_right);

        if (exposed_width != 0U) {
            window_mark_rect_locked(
                (int32_t)overlap_right,
                old_y,
                exposed_width,
                height);
        }
    }

    /*
     * Vertical exposed strip only covers horizontal overlap, so it cannot
     * overlap the strip above.
     */
    {
        uint32_t overlap_width =
            (uint32_t)(
                overlap_right -
                overlap_left);

        if (new_top > old_top) {
            uint32_t exposed_height =
                (uint32_t)(
                    overlap_top -
                    old_top);

            if (overlap_width != 0U &&
                exposed_height != 0U) {

                window_mark_rect_locked(
                    (int32_t)overlap_left,
                    old_y,
                    overlap_width,
                    exposed_height);
            }
        } else if (new_top < old_top) {
            uint32_t exposed_height =
                (uint32_t)(
                    old_bottom -
                    overlap_bottom);

            if (overlap_width != 0U &&
                exposed_height != 0U) {

                window_mark_rect_locked(
                    (int32_t)overlap_left,
                    (int32_t)overlap_bottom,
                    overlap_width,
                    exposed_height);
            }
        }
    }

    /*
     * The complete final window position must still be rendered.
     */
    window_mark_rect_locked(
        new_x,
        new_y,
        width,
        height);
}

static void window_mark_moved_cursor_locked(uint32_t old_x, uint32_t old_y,
                                            uint32_t new_x, uint32_t new_y) {
    int32_t old_left;
    int32_t old_top;
    int32_t new_left;
    int32_t new_top;

    /* pointer_x/y 是 cursor hotspot，不是 sprite 左上角。 */
    old_left = (int32_t)old_x - (int32_t)WINDOW_CURSOR_HOTSPOT_X;
    old_top = (int32_t)old_y - (int32_t)WINDOW_CURSOR_HOTSPOT_Y;
    new_left = (int32_t)new_x - (int32_t)WINDOW_CURSOR_HOTSPOT_X;
    new_top = (int32_t)new_y - (int32_t)WINDOW_CURSOR_HOTSPOT_Y;

    /*
     * Old/new cursor locations are independent damages.  Do not create a
     * potentially screen-sized bounding box when the pointer jumps.
     */
    window_mark_rect_locked(old_left, old_top,
                            WINDOW_CURSOR_WIDTH, WINDOW_CURSOR_HEIGHT);

    if (old_left != new_left || old_top != new_top) {
        window_mark_rect_locked(new_left, new_top,
                                WINDOW_CURSOR_WIDTH, WINDOW_CURSOR_HEIGHT);
    }
}

/* 一次鼠标事务只允许一次 framebuffer 区域提交。焦点、任务栏、被拖动
 * 窗口和光标可能分别产生 damage；分开提交会让 GOP 扫描到半帧状态。 */
static void window_coalesce_damage_locked(void) {
    uint32_t first = 0U;

    if (g_window_server.damage_full ||
        g_window_server.damage_count <= 1U) {
        return;
    }

    /*
     * Keep independent damage rectangles independent.  Merge a pair only
     * when the bounding rectangle adds <=25% wasted pixels over their real
     * union.  At most WINDOW_DAMAGE_MAX_RECTS exist, so this O(n^2) pass is
     * tiny compared with repainting a needlessly large framebuffer region.
     */
    while (first < g_window_server.damage_count) {
        uint32_t second = first + 1U;
        bool merged_any = false;

        while (second < g_window_server.damage_count) {
            window_damage_rect_t *a =
                &g_window_server.damage_rects[first];
            const window_damage_rect_t *b =
                &g_window_server.damage_rects[second];

            uint32_t left = a->left < b->left ? a->left : b->left;
            uint32_t top = a->top < b->top ? a->top : b->top;
            uint32_t right = a->right > b->right ? a->right : b->right;
            uint32_t bottom =
                a->bottom > b->bottom ? a->bottom : b->bottom;

            uint32_t overlap_left =
                a->left > b->left ? a->left : b->left;
            uint32_t overlap_top =
                a->top > b->top ? a->top : b->top;
            uint32_t overlap_right =
                a->right < b->right ? a->right : b->right;
            uint32_t overlap_bottom =
                a->bottom < b->bottom ? a->bottom : b->bottom;

            uint64_t area_a =
                (uint64_t)(a->right - a->left) *
                (a->bottom - a->top);
            uint64_t area_b =
                (uint64_t)(b->right - b->left) *
                (b->bottom - b->top);
            uint64_t overlap_area = 0U;
            uint64_t union_area;
            uint64_t merged_area;

            if (overlap_left < overlap_right &&
                overlap_top < overlap_bottom) {
                overlap_area =
                    (uint64_t)(overlap_right - overlap_left) *
                    (overlap_bottom - overlap_top);
            }

            union_area = area_a + area_b - overlap_area;
            merged_area =
                (uint64_t)(right - left) *
                (bottom - top);

            if (merged_area <= union_area + union_area / 4U) {
                a->left = left;
                a->top = top;
                a->right = right;
                a->bottom = bottom;

                for (uint32_t index = second + 1U;
                     index < g_window_server.damage_count;
                     ++index) {
                    g_window_server.damage_rects[index - 1U] =
                        g_window_server.damage_rects[index];
                }

                --g_window_server.damage_count;
                merged_any = true;

                /*
                 * a changed, so retry it against every remaining rectangle.
                 */
                second = first + 1U;
                continue;
            }

            ++second;
        }

        if (!merged_any) {
            ++first;
        } else {
            /*
             * No pair with this newly expanded rectangle remains mergeable
             * after the restarted scan.
             */
            ++first;
        }
    }
}

/*
 * 圆角半径固定为 WINDOW_CORNER_RADIUS，因此不在合成热路径中计算平方和。
 * 每一项表示该扫描线从左、右各裁掉的像素数；下半部分反向索引即可。
 */
static const uint8_t g_window_corner_inset[WINDOW_CORNER_RADIUS] = {
    4U, 3U, 2U, 1U, 1U, 0U,
};

static uint32_t compositor_corner_inset(uint32_t row, uint32_t width,
                                        uint32_t height) {
    if (width < WINDOW_CORNER_RADIUS * 2U ||
        height < WINDOW_CORNER_RADIUS * 2U || row >= height) return 0U;
    if (row < WINDOW_CORNER_RADIUS) return g_window_corner_inset[row];
    row = height - 1U - row;
    if (row < WINDOW_CORNER_RADIUS) return g_window_corner_inset[row];
    return 0U;
}


/*
 * Fast solid-color fill into the normal WB composite framebuffer.
 *
 * Two XRGB8888 pixels are packed into one 64-bit value and REP STOSQ handles
 * the bulk span. No XMM/YMM/FPU state is touched.
 */
static inline void compositor_fill_span_wb(
    volatile uint32_t *destination,
    uint32_t pixels,
    uint32_t color) {

    uint64_t pattern;
    uint64_t qwords;
    void *out;

    if (destination == 0 || pixels == 0U) {
        return;
    }

    /*
     * Align the destination to 8 bytes first.
     */
    if ((((uintptr_t)destination) & 7U) != 0U) {
        *destination++ = color;
        --pixels;

        if (pixels == 0U) {
            return;
        }
    }

    pattern =
        (uint64_t)color |
        ((uint64_t)color << 32U);

    qwords = pixels >> 1U;

    if (qwords != 0U) {
        out = (void *)(uintptr_t)destination;

        __asm__ volatile (
            "rep stosq"
            : "+D"(out),
              "+c"(qwords)
            : "a"(pattern)
            : "memory");

        destination =
            (volatile uint32_t *)(uintptr_t)out;
    }

    /*
     * Odd pixel tail.
     */
    if ((pixels & 1U) != 0U) {
        *destination = color;
    }
}


static void compositor_fill_rounded_locked(int32_t x, int32_t y,
                                           uint32_t width, uint32_t height,
                                           uint32_t radius, uint32_t color);

static void compositor_fill_locked(int32_t x, int32_t y, uint32_t width,
                                    uint32_t height, uint32_t color) {
    compositor_fill_rounded_locked(x, y, width, height, 0U, color);
}

static void compositor_fill_rounded_locked(int32_t x, int32_t y,
                                           uint32_t width, uint32_t height,
                                           uint32_t radius, uint32_t color) {
    int32_t left = x < 0 ? 0 : x;
    int32_t top = y < 0 ? 0 : y;
    int64_t right = (int64_t)x + width;
    int64_t bottom = (int64_t)y + height;
    if (right > (int64_t)g_window_server.display_width) {
        right = g_window_server.display_width;
    }
    if (bottom > (int64_t)g_window_server.display_height) {
        bottom = g_window_server.display_height;
    }
    if (left < (int32_t)g_compositor_snapshot.damage_left) {
        left = (int32_t)g_compositor_snapshot.damage_left;
    }
    if (top < (int32_t)g_compositor_snapshot.damage_top) {
        top = (int32_t)g_compositor_snapshot.damage_top;
    }
    if (right > (int64_t)g_compositor_snapshot.damage_right) {
        right = g_compositor_snapshot.damage_right;
    }
    if (bottom > (int64_t)g_compositor_snapshot.damage_bottom) {
        bottom = g_compositor_snapshot.damage_bottom;
    }
    if (left >= right || top >= bottom) return;
    for (int32_t row = top; row < bottom; ++row) {
        uint32_t inset = 0U;
        int64_t relative_row = (int64_t)row - y;
        int64_t span_left;
        int64_t span_right;
        if (radius == WINDOW_CORNER_RADIUS && relative_row >= 0 &&
            relative_row < (int64_t)height) {
            inset = compositor_corner_inset((uint32_t)relative_row, width, height);
        }
        span_left = (int64_t)x + inset;
        span_right = (int64_t)x + width - inset;
        if (span_left < left) span_left = left;
        if (span_right > right) span_right = right;
        if (span_left >= span_right) continue;
        volatile uint32_t *destination =
            g_window_server.composite_framebuffer +
            (uint64_t)(uint32_t)row *
                g_window_server.display_stride +
            (uint32_t)span_left;

        compositor_fill_span_wb(
            destination,
            (uint32_t)(span_right - span_left),
            color);
    }
}


typedef struct desktop_icon_entry {
    uint32_t app;
    int32_t x;
    int32_t y;
    const char *label;
} desktop_icon_entry_t;

static const desktop_icon_entry_t g_desktop_icons[] = {
    { DESKTOP_APP_FILES,    DESKTOP_ICON_START_X, DESKTOP_ICON_START_Y, "Files" },
    { DESKTOP_APP_TERMINAL, DESKTOP_ICON_START_X,
      DESKTOP_ICON_START_Y + (int32_t)(DESKTOP_ICON_CELL_HEIGHT + DESKTOP_ICON_GAP_Y),
      "Terminal" },
    { DESKTOP_APP_NOTES,    DESKTOP_ICON_START_X,
      DESKTOP_ICON_START_Y + (int32_t)((DESKTOP_ICON_CELL_HEIGHT + DESKTOP_ICON_GAP_Y) * 2U),
      "Notes" },
    { DESKTOP_APP_NETWORK,  DESKTOP_ICON_START_X,
      DESKTOP_ICON_START_Y + (int32_t)((DESKTOP_ICON_CELL_HEIGHT + DESKTOP_ICON_GAP_Y) * 3U),
      "Network" },
};


/*
 * Modern centered dock layout.
 *
 * g_desktop_icons keeps application metadata, while geometry is calculated
 * from the live display size. Hit testing, damage and rendering therefore
 * always use exactly the same rectangle.
 */
static bool desktop_icon_layout_locked(
    uint32_t index,
    int32_t *out_x,
    int32_t *out_y) {

    const uint32_t icon_count =
        (uint32_t)(
            sizeof(g_desktop_icons) /
            sizeof(g_desktop_icons[0]));

    uint32_t content_width;
    uint32_t dock_width;

    int32_t dock_x;
    int32_t dock_y;

    if (index >= icon_count ||
        out_x == 0 ||
        out_y == 0 ||
        icon_count == 0U) {
        return false;
    }

    content_width =
        icon_count *
            DESKTOP_ICON_CELL_WIDTH +
        (icon_count - 1U) *
            DESKTOP_DOCK_ICON_GAP;

    dock_width =
        content_width +
        DESKTOP_DOCK_PADDING_X * 2U;

    if (g_window_server.display_width <
            dock_width + 16U ||
        g_window_server.display_height <
            DESKTOP_TOPBAR_HEIGHT +
            DESKTOP_DOCK_HEIGHT +
            DESKTOP_DOCK_BOTTOM +
            32U) {
        return false;
    }

    dock_x =
        (int32_t)(
            g_window_server.display_width -
            dock_width) /
        2;

    dock_y =
        (int32_t)
            g_window_server.display_height -
        (int32_t)DESKTOP_DOCK_HEIGHT -
        (int32_t)DESKTOP_DOCK_BOTTOM;

    *out_x =
        dock_x +
        (int32_t)DESKTOP_DOCK_PADDING_X +
        (int32_t)(
            index *
            (DESKTOP_ICON_CELL_WIDTH +
             DESKTOP_DOCK_ICON_GAP));

    *out_y =
        dock_y +
        (int32_t)(
            DESKTOP_DOCK_HEIGHT -
            DESKTOP_ICON_CELL_HEIGHT) /
        2;

    return true;
}


static uint32_t desktop_text_length(const char *text) {
    uint32_t length = 0U;
    if (text == 0) return 0U;
    while (text[length] != '\0') ++length;
    return length;
}

static void desktop_put_pixel_locked(int32_t x, int32_t y, uint32_t color) {
    if (g_window_server.composite_framebuffer == 0 ||
        x < 0 || y < 0 ||
        x >= (int32_t)g_window_server.display_width ||
        y >= (int32_t)g_window_server.display_height ||
        x < (int32_t)g_compositor_snapshot.damage_left ||
        y < (int32_t)g_compositor_snapshot.damage_top ||
        x >= (int32_t)g_compositor_snapshot.damage_right ||
        y >= (int32_t)g_compositor_snapshot.damage_bottom) {
        return;
    }
    g_window_server.composite_framebuffer[
        (uint64_t)(uint32_t)y * g_window_server.display_stride +
        (uint32_t)x] = color;
}

static void desktop_draw_small_glyph_locked(int32_t x, int32_t y,
                                            char character, uint32_t color) {
    const UINT8 *glyph = ascii_font_glyph((UINT8)character);
    if (glyph == 0) return;
    for (uint32_t row = 0U; row < 16U; ++row) {
        uint32_t offset = row * 4U;
        uint16_t bits =
            (uint16_t)(((uint16_t)glyph[offset] << 8) | glyph[offset + 1U]);
        bits |= (uint16_t)(((uint16_t)glyph[offset + 2U] << 8) |
                           glyph[offset + 3U]);
        for (uint32_t column = 0U; column < 8U; ++column) {
            if ((bits & (uint16_t)(0xC000U >> (column * 2U))) != 0U) {
                desktop_put_pixel_locked(x + (int32_t)column,
                                         y + (int32_t)row, color);
            }
        }
    }
}

static void desktop_draw_text_locked(int32_t x, int32_t y,
                                     const char *text, uint32_t color) {
    if (text == 0) return;
    for (uint32_t index = 0U; text[index] != '\0'; ++index) {
        desktop_draw_small_glyph_locked(
            x + (int32_t)(index * 8U), y, text[index], color);
    }
}

static uint32_t desktop_app_at_locked(
    uint32_t x,
    uint32_t y) {

    for (uint32_t index = 0U;
         index <
             sizeof(g_desktop_icons) /
             sizeof(g_desktop_icons[0]);
         ++index) {

        int32_t icon_x;
        int32_t icon_y;

        if (!desktop_icon_layout_locked(
                index,
                &icon_x,
                &icon_y)) {
            continue;
        }

        if ((int64_t)x >= icon_x &&
            (int64_t)y >= icon_y &&

            (int64_t)x <
                (int64_t)icon_x +
                DESKTOP_ICON_CELL_WIDTH &&

            (int64_t)y <
                (int64_t)icon_y +
                DESKTOP_ICON_CELL_HEIGHT) {

            return
                g_desktop_icons[index].app;
        }
    }

    return DESKTOP_APP_NONE;
}

static void desktop_mark_app_locked(
    uint32_t app) {

    if (app == DESKTOP_APP_NONE) {
        return;
    }

    for (uint32_t index = 0U;
         index <
             sizeof(g_desktop_icons) /
             sizeof(g_desktop_icons[0]);
         ++index) {

        int32_t icon_x;
        int32_t icon_y;

        if (g_desktop_icons[index].app !=
            app) {
            continue;
        }

        if (!desktop_icon_layout_locked(
                index,
                &icon_x,
                &icon_y)) {
            return;
        }

        window_mark_rect_locked(
            icon_x,
            icon_y,
            DESKTOP_ICON_CELL_WIDTH,
            DESKTOP_ICON_CELL_HEIGHT);

        return;
    }
}

static void desktop_draw_folder_icon_locked(int32_t x, int32_t y) {
    compositor_fill_rounded_locked(x + 2, y + 13, 44U, 31U,
                                   WINDOW_CORNER_RADIUS, 0x00D99F36U);
    compositor_fill_locked(x + 6, y + 8, 17U, 10U, 0x00F2C66DU);
    compositor_fill_rounded_locked(x, y + 16, 48U, 30U,
                                   WINDOW_CORNER_RADIUS, 0x00F0B84FU);
    compositor_fill_locked(x + 4, y + 21, 40U, 3U, 0x00F7CF79U);
}

static void desktop_draw_terminal_icon_locked(int32_t x, int32_t y) {
    compositor_fill_rounded_locked(x, y + 3, 48U, 42U,
                                   WINDOW_CORNER_RADIUS, 0x000B1420U);
    compositor_fill_locked(x + 4, y + 7, 40U, 4U, 0x002A526FU);
    for (uint32_t step = 0U; step < 7U; ++step) {
        compositor_fill_locked(x + 10 + (int32_t)step,
                               y + 17 + (int32_t)step, 2U, 2U,
                               0x007FE0AEU);
        compositor_fill_locked(x + 10 + (int32_t)step,
                               y + 29 - (int32_t)step, 2U, 2U,
                               0x007FE0AEU);
    }
    compositor_fill_locked(x + 27, y + 31, 11U, 2U, 0x00C8E7F0U);
}

static void desktop_draw_notes_icon_locked(int32_t x, int32_t y) {
    compositor_fill_rounded_locked(x + 5, y + 2, 38U, 44U,
                                   WINDOW_CORNER_RADIUS, 0x00EAF2F6U);
    compositor_fill_locked(x + 5, y + 2, 38U, 7U, 0x005DADE2U);
    compositor_fill_locked(x + 12, y + 16, 24U, 2U, 0x006F8797U);
    compositor_fill_locked(x + 12, y + 23, 24U, 2U, 0x006F8797U);
    compositor_fill_locked(x + 12, y + 30, 20U, 2U, 0x006F8797U);
    compositor_fill_locked(x + 12, y + 37, 16U, 2U, 0x006F8797U);
}


static void desktop_draw_network_icon_locked(int32_t x, int32_t y) {
    compositor_fill_rounded_locked(x + 4, y + 4, 40U, 40U,
                                   WINDOW_CORNER_RADIUS, 0x00152A38U);
    compositor_fill_locked(x + 22, y + 12, 4U, 20U, 0x007FD7E8U);
    compositor_fill_locked(x + 14, y + 18, 20U, 4U, 0x007FD7E8U);
    compositor_fill_rounded_locked(x + 8, y + 14, 12U, 12U,
                                   WINDOW_CORNER_RADIUS, 0x005DADE2U);
    compositor_fill_rounded_locked(x + 28, y + 14, 12U, 12U,
                                   WINDOW_CORNER_RADIUS, 0x005DADE2U);
    compositor_fill_rounded_locked(x + 18, y + 30, 12U, 12U,
                                   WINDOW_CORNER_RADIUS, 0x007FE0AEU);
}

static void desktop_draw_icon_locked(
    uint32_t index,
    const desktop_icon_entry_t *icon) {

    uint32_t text_width;

    int32_t cell_x;
    int32_t cell_y;

    int32_t image_x;
    int32_t image_y;
    int32_t text_x;

    bool hovered;

    if (icon == 0 ||
        !desktop_icon_layout_locked(
            index,
            &cell_x,
            &cell_y)) {
        return;
    }

    hovered =
        g_compositor_snapshot.desktop_hovered_app ==
        icon->app;

    /*
     * Modern launcher card.
     *
     * Static cache stores the normal state. On hover this card is simply
     * painted over the cached icon and then the icon itself is redrawn.
     */
    if (hovered) {
        compositor_fill_rounded_locked(
            cell_x,
            cell_y,
            DESKTOP_ICON_CELL_WIDTH,
            DESKTOP_ICON_CELL_HEIGHT,
            WINDOW_CORNER_RADIUS,
            0x00263A53U);

        compositor_fill_rounded_locked(
            cell_x + 1,
            cell_y + 1,
            DESKTOP_ICON_CELL_WIDTH - 2U,
            DESKTOP_ICON_CELL_HEIGHT - 2U,
            WINDOW_CORNER_RADIUS,
            0x00203349U);

        /*
         * Accent indicator.
         */
        compositor_fill_rounded_locked(
            cell_x +
                (int32_t)
                    (DESKTOP_ICON_CELL_WIDTH - 24U) /
                    2,
            cell_y + 2,
            24U,
            2U,
            WINDOW_CORNER_RADIUS,
            0x006EA8FEU);
    }

    image_x =
        cell_x +
        (int32_t)(
            DESKTOP_ICON_CELL_WIDTH -
            DESKTOP_ICON_IMAGE_WIDTH) /
        2;

    image_y =
        cell_y + 3;

    if (icon->app == DESKTOP_APP_FILES) {
        desktop_draw_folder_icon_locked(
            image_x,
            image_y);

    } else if (
        icon->app ==
        DESKTOP_APP_TERMINAL) {

        desktop_draw_terminal_icon_locked(
            image_x,
            image_y);

    } else if (
        icon->app ==
        DESKTOP_APP_NOTES) {

        desktop_draw_notes_icon_locked(
            image_x,
            image_y);

    } else if (
        icon->app ==
        DESKTOP_APP_NETWORK) {

        desktop_draw_network_icon_locked(
            image_x,
            image_y);
    }

    text_width =
        desktop_text_length(icon->label) *
        8U;

    text_x =
        cell_x +
        (int32_t)(
            DESKTOP_ICON_CELL_WIDTH -
            (text_width <
                     DESKTOP_ICON_CELL_WIDTH ?
                 text_width :
                 DESKTOP_ICON_CELL_WIDTH)) /
        2;

    desktop_draw_text_locked(
        text_x,
        cell_y + 50,
        icon->label,
        hovered ?
            0x00F5F8FCU :
            0x00B7C5D6U);
}

static void desktop_render_static_locked(void) {
    uint32_t height =
        g_window_server.display_height;

    uint32_t width =
        g_window_server.display_width;

    uint32_t left =
        g_compositor_snapshot.damage_left;

    uint32_t right =
        g_compositor_snapshot.damage_right;

    uint32_t top =
        g_compositor_snapshot.damage_top;

    uint32_t bottom =
        g_compositor_snapshot.damage_bottom;

    const uint32_t icon_count =
        (uint32_t)(
            sizeof(g_desktop_icons) /
            sizeof(g_desktop_icons[0]));

    if (g_window_server.composite_framebuffer == 0 ||
        left >= right ||
        top >= bottom) {
        return;
    }

    /*
     * Deep navy modern wallpaper.
     *
     * Keep it row-based so the allocation-failure fallback remains cheap.
     */
    for (uint32_t row = top;
         row < bottom;
         ++row) {

        uint32_t t =
            height > 1U ?
            (uint32_t)(
                ((uint64_t)row * 255U) /
                (height - 1U)) :
            0U;

        uint32_t red =
            6U +
            (10U * t) /
            255U;

        uint32_t green =
            13U +
            (22U * t) /
            255U;

        uint32_t blue =
            27U +
            (31U * t) /
            255U;

        /*
         * Slightly cooler lower third.
         */
        if (row >
            (height * 2U) / 3U) {

            blue += 4U;
            green += 2U;
        }

        uint32_t color =
            (red << 16) |
            (green << 8) |
            blue;

        volatile uint32_t *destination =
            g_window_server.composite_framebuffer +
            (uint64_t)row *
                g_window_server.display_stride +
            left;

        compositor_fill_span_wb(
            destination,
            right - left,
            color);
    }

    /*
     * Subtle wallpaper accent bands.
     */
    if (width > 800U &&
        height > 500U) {

        compositor_fill_rounded_locked(
            (int32_t)width - 620,
            (int32_t)height / 3,
            420U,
            2U,
            WINDOW_CORNER_RADIUS,
            0x001C4666U);

        compositor_fill_rounded_locked(
            (int32_t)width - 540,
            (int32_t)height / 3 + 24,
            310U,
            1U,
            WINDOW_CORNER_RADIUS,
            0x00215A78U);

        compositor_fill_rounded_locked(
            150,
            (int32_t)height - 220,
            280U,
            1U,
            WINDOW_CORNER_RADIUS,
            0x001B4968U);
    }

    /*
     * Top bar.
     */
    compositor_fill_locked(
        0,
        0,
        width,
        DESKTOP_TOPBAR_HEIGHT,
        0x000A1220U);

    compositor_fill_locked(
        0,
        (int32_t)DESKTOP_TOPBAR_HEIGHT - 1,
        width,
        1U,
        0x00213247U);

    /*
     * LiteOS logo tile.
     */
    compositor_fill_rounded_locked(
        14,
        8,
        24U,
        22U,
        WINDOW_CORNER_RADIUS,
        0x006EA8FEU);

    desktop_draw_text_locked(
        22,
        11,
        "L",
        0x00FFFFFFU);

    desktop_draw_text_locked(
        48,
        11,
        "LiteOS",
        0x00F1F5F9U);

    /*
     * Center workspace pill.
     */
    if (width > 400U) {
        int32_t workspace_x =
            (int32_t)width / 2 -
            56;

        compositor_fill_rounded_locked(
            workspace_x,
            7,
            112U,
            24U,
            WINDOW_CORNER_RADIUS,
            0x00131F30U);

        desktop_draw_text_locked(
            workspace_x + 12,
            11,
            "Workspace 1",
            0x00AFC0D2U);
    }

    /*
     * Right-side healthy system indicator.
     */
    if (width > 220U) {
        compositor_fill_rounded_locked(
            (int32_t)width - 86,
            15,
            8U,
            8U,
            WINDOW_CORNER_RADIUS,
            0x004ADE80U);

        desktop_draw_text_locked(
            (int32_t)width - 70,
            11,
            "Ready",
            0x00AFC0D2U);
    }

    /*
     * Floating glass-like dock.
     *
     * We do not require alpha blending here: layered solid surfaces provide a
     * clean glass-panel appearance while keeping desktop caching extremely
     * cheap.
     */
    if (icon_count != 0U) {
        uint32_t content_width =
            icon_count *
                DESKTOP_ICON_CELL_WIDTH +
            (icon_count - 1U) *
                DESKTOP_DOCK_ICON_GAP;

        uint32_t dock_width =
            content_width +
            DESKTOP_DOCK_PADDING_X * 2U;

        if (width >= dock_width + 16U &&
            height >
                DESKTOP_DOCK_HEIGHT +
                DESKTOP_DOCK_BOTTOM +
                DESKTOP_TOPBAR_HEIGHT) {

            int32_t dock_x =
                (int32_t)(
                    width -
                    dock_width) /
                2;

            int32_t dock_y =
                (int32_t)height -
                (int32_t)DESKTOP_DOCK_HEIGHT -
                (int32_t)DESKTOP_DOCK_BOTTOM;

            /*
             * Shadow.
             */
            compositor_fill_rounded_locked(
                dock_x + 3,
                dock_y + 5,
                dock_width,
                DESKTOP_DOCK_HEIGHT,
                WINDOW_CORNER_RADIUS,
                0x0004080FU);

            /*
             * Outer shell.
             */
            compositor_fill_rounded_locked(
                dock_x,
                dock_y,
                dock_width,
                DESKTOP_DOCK_HEIGHT,
                WINDOW_CORNER_RADIUS,
                0x00182738U);

            /*
             * Inner glass surface.
             */
            compositor_fill_rounded_locked(
                dock_x + 1,
                dock_y + 1,
                dock_width - 2U,
                DESKTOP_DOCK_HEIGHT - 2U,
                WINDOW_CORNER_RADIUS,
                0x00111D2CU);

            /*
             * Highlight along upper edge.
             */
            compositor_fill_locked(
                dock_x + 14,
                dock_y + 4,
                dock_width - 28U,
                1U,
                0x002A4057U);
        }
    }

    /*
     * App launchers.
     */
    for (uint32_t index = 0U;
         index <
             sizeof(g_desktop_icons) /
             sizeof(g_desktop_icons[0]);
         ++index) {

        desktop_draw_icon_locked(
            index,
            &g_desktop_icons[index]);
    }
}


/*
 * Retained desktop backing surface.
 *
 * The wallpaper, top bar, dock and normal icon state are static. Rebuilding
 * them for every exposed window rectangle wastes substantial CPU time during
 * dragging.
 */
static uint32_t *g_desktop_cache;
static uint32_t g_desktop_cache_width;
static uint32_t g_desktop_cache_height;
static uint32_t g_desktop_cache_stride;
static bool g_desktop_cache_ready;


/*
 * WB -> WB scanline copy used by the retained desktop.
 *
 * No SIMD/FPU state is touched.
 */
static inline void desktop_copy_wb_pixels(
    volatile uint32_t *destination,
    const uint32_t *source,
    uint32_t pixels) {

    uint64_t qwords;

    if (destination == 0 ||
        source == 0 ||
        pixels == 0U) {
        return;
    }

    qwords = pixels >> 1U;

    if (qwords != 0U) {
        void *out =
            (void *)(uintptr_t)destination;

        const void *in =
            (const void *)source;

        uint64_t count = qwords;

        __asm__ volatile (
            "rep movsq"
            : "+D"(out),
              "+S"(in),
              "+c"(count)
            :
            : "memory");
    }

    if ((pixels & 1U) != 0U) {
        destination[pixels - 1U] =
            source[pixels - 1U];
    }
}


/*
 * Build the static desktop exactly once.
 *
 * desktop_render_static_locked() already knows how to render the wallpaper,
 * topbar, dock and icons. Temporarily redirect its destination into the cache
 * and force the hover state to NONE.
 */
static bool desktop_build_cache(void) {
    volatile uint32_t *saved_framebuffer;

    uint32_t saved_left;
    uint32_t saved_top;
    uint32_t saved_right;
    uint32_t saved_bottom;
    uint32_t saved_hover;

    uint64_t bytes;

    if (g_window_server.display_width == 0U ||
        g_window_server.display_height == 0U ||
        g_window_server.display_stride <
            g_window_server.display_width) {
        return false;
    }

    /*
     * Rebuild if a future display-mode change modifies geometry.
     */
    if (g_desktop_cache != 0 &&
        (g_desktop_cache_width !=
             g_window_server.display_width ||
         g_desktop_cache_height !=
             g_window_server.display_height ||
         g_desktop_cache_stride !=
             g_window_server.display_stride)) {

        kfree(g_desktop_cache);

        g_desktop_cache = 0;
        g_desktop_cache_ready = false;
    }

    if (g_desktop_cache_ready &&
        g_desktop_cache != 0) {
        return true;
    }

    bytes =
        (uint64_t)g_window_server.display_stride *
        g_window_server.display_height *
        sizeof(uint32_t);

    if (bytes == 0U ||
        bytes > (uint64_t)SIZE_MAX) {
        return false;
    }

    if (g_desktop_cache == 0) {
        g_desktop_cache =
            (uint32_t *)kzalloc(
                (size_t)bytes,
                0);

        if (g_desktop_cache == 0) {
            return false;
        }
    }

    saved_framebuffer =
        g_window_server.composite_framebuffer;

    saved_left =
        g_compositor_snapshot.damage_left;

    saved_top =
        g_compositor_snapshot.damage_top;

    saved_right =
        g_compositor_snapshot.damage_right;

    saved_bottom =
        g_compositor_snapshot.damage_bottom;

    saved_hover =
        g_compositor_snapshot.desktop_hovered_app;

    /*
     * Render an unhovered full-screen desktop into the retained buffer.
     */
    g_window_server.composite_framebuffer =
        (volatile uint32_t *)g_desktop_cache;

    g_compositor_snapshot.damage_left = 0U;
    g_compositor_snapshot.damage_top = 0U;

    g_compositor_snapshot.damage_right =
        g_window_server.display_width;

    g_compositor_snapshot.damage_bottom =
        g_window_server.display_height;

    g_compositor_snapshot.desktop_hovered_app =
        DESKTOP_APP_NONE;

    desktop_render_static_locked();

    /*
     * Restore live frame state.
     */
    g_window_server.composite_framebuffer =
        saved_framebuffer;

    g_compositor_snapshot.damage_left =
        saved_left;

    g_compositor_snapshot.damage_top =
        saved_top;

    g_compositor_snapshot.damage_right =
        saved_right;

    g_compositor_snapshot.damage_bottom =
        saved_bottom;

    g_compositor_snapshot.desktop_hovered_app =
        saved_hover;

    g_desktop_cache_width =
        g_window_server.display_width;

    g_desktop_cache_height =
        g_window_server.display_height;

    g_desktop_cache_stride =
        g_window_server.display_stride;

    g_desktop_cache_ready = true;

    return true;
}


/*
 * Restore only the current damaged desktop rectangle.
 */
static void desktop_copy_cached_region(void) {
    uint32_t left =
        g_compositor_snapshot.damage_left;

    uint32_t top =
        g_compositor_snapshot.damage_top;

    uint32_t right =
        g_compositor_snapshot.damage_right;

    uint32_t bottom =
        g_compositor_snapshot.damage_bottom;

    if (g_desktop_cache == 0 ||
        g_window_server.composite_framebuffer == 0 ||
        left >= right ||
        top >= bottom) {
        return;
    }

    for (uint32_t row = top;
         row < bottom;
         ++row) {

        volatile uint32_t *destination =
            g_window_server.composite_framebuffer +

            (uint64_t)row *
                g_window_server.display_stride +

            left;

        const uint32_t *source =
            g_desktop_cache +

            (uint64_t)row *
                g_desktop_cache_stride +

            left;

        desktop_copy_wb_pixels(
            destination,
            source,
            right - left);
    }
}


/*
 * The retained cache contains icons in their normal state.
 *
 * Hover is dynamic, so redraw only the currently hovered icon after restoring
 * the static desktop. The old hovered icon is automatically erased because
 * its damage rectangle is first restored from g_desktop_cache.
 */
static void desktop_draw_hover_locked(void) {
    uint32_t hovered =
        g_compositor_snapshot.desktop_hovered_app;

    if (hovered == DESKTOP_APP_NONE) {
        return;
    }

    for (uint32_t index = 0U;
         index <
             sizeof(g_desktop_icons) /
             sizeof(g_desktop_icons[0]);
         ++index) {

        if (g_desktop_icons[index].app !=
            hovered) {
            continue;
        }

        desktop_draw_icon_locked(
            index,
            &g_desktop_icons[index]);

        return;
    }
}


/*
 * Fast desktop composition path.
 */
static void desktop_draw_wallpaper_locked(void) {
    if (!desktop_build_cache()) {
        /*
         * Allocation failure fallback preserves the old correct behavior.
         */
        desktop_render_static_locked();
        return;
    }

    desktop_copy_cached_region();

    /*
     * Only dynamic desktop state remaining today is icon hover.
     */
    desktop_draw_hover_locked();
}


static void desktop_cycle_window_focus(void) {
    uint32_t first_identifier = 0U;
    uint32_t next_identifier = 0U;
    bool choose_next = false;

    for (uint32_t index = 0U; ; ++index) {
        window_server_snapshot_t snapshot;
        kstatus_t status = window_server_snapshot(index, &snapshot);
        if (status != K_OK) break;
        if (!snapshot.visible || snapshot.identifier == 0U) continue;

        if (first_identifier == 0U) {
            first_identifier = snapshot.identifier;
        }
        if (choose_next) {
            next_identifier = snapshot.identifier;
            break;
        }
        if (snapshot.focused) {
            choose_next = true;
        }
    }

    if (next_identifier == 0U) next_identifier = first_identifier;
    if (next_identifier != 0U) {
        (void)window_server_focus(next_identifier);
    }
}

static const char *desktop_program_path(uint32_t app) {
    if (app == DESKTOP_APP_FILES) return "/sbin/fileman";
    if (app == DESKTOP_APP_TERMINAL) return "/sbin/gshell";
    if (app == DESKTOP_APP_NOTES) return "/sbin/notepad";
    if (app == DESKTOP_APP_NETWORK) return "/sbin/netmgr";
    return 0;
}


static const char *desktop_app_window_title(
    uint32_t app) {

    if (app == DESKTOP_APP_FILES) {
        return "FILEMAN";
    }

    if (app == DESKTOP_APP_TERMINAL) {
        return "SHELL";
    }

    if (app == DESKTOP_APP_NOTES) {
        return "NOTEPAD";
    }

    if (app == DESKTOP_APP_NETWORK) {
        return "NETWORK";
    }

    return 0;
}


static bool window_title_matches(
    const char *title,
    const char *expected) {

    uint32_t index = 0U;

    if (title == 0 ||
        expected == 0) {
        return false;
    }

    while (index < 32U) {
        if (title[index] != expected[index]) {
            return false;
        }

        if (expected[index] == '\0') {
            return true;
        }

        ++index;
    }

    return false;
}


/*
 * Dock activation first restores an existing minimized instance.
 *
 * Only if no minimized window for this desktop application exists does the
 * normal launcher create another process.
 */
static bool desktop_restore_minimized_app(
    uint32_t app) {

    const char *expected =
        desktop_app_window_title(app);

    window_server_window_t *window = 0;

    if (expected == 0) {
        return false;
    }

    window_lock();

    for (uint32_t index = g_window_server.count;
         index != 0U;
         --index) {

        window_server_window_t *candidate =
            g_window_server.windows[index - 1U];

        if (candidate == 0 ||
            !candidate->minimized ||
            (candidate->flags &
             OS_WINDOW_VISIBLE) == 0U ||
            !window_title_matches(
                candidate->title,
                expected)) {

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

    /*
     * focus_locked() also raises it to the top of Z-order and damages both
     * old/new focused decoration.
     */
    focus_locked(window);

    window_mark_window_locked(
        window);

    window_unlock();

    (void)wake_all(
        &g_window_server.event_waitq);

    return true;
}


static kstatus_t desktop_launch_program(uint32_t app) {
    const char *path = desktop_program_path(app);
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

    status = vfs_open_kernel(path, VFS_OPEN_READ, &file);
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
    file = 0;
    if (status != K_OK || bytes != image_size) {
        kfree(image);
        return status == K_OK ? K_EIO : status;
    }

    status = process_create(0, &process);
    if (status == K_OK) {
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
        if (process != 0) object_put(process);
        return status;
    }

    object_put(thread);
    object_put(process);
    return K_OK;
}

static window_server_window_t *window_at_locked(uint32_t x, uint32_t y) {
    for (uint32_t index = g_window_server.count; index != 0U; --index) {
        window_server_window_t *window = g_window_server.windows[index - 1U];
        int64_t relative_x;
        int64_t relative_y;
        uint32_t inset;
        if (window == 0 ||
            window->minimized ||
            (window->flags & OS_WINDOW_VISIBLE) == 0U) {
            continue;
        }
        relative_x = (int64_t)x - window->x;
        relative_y = (int64_t)y - window->y;
        if (relative_x < 0 || relative_y < 0 ||
            relative_x >= (int64_t)window_outer_width(window->width, window->flags) ||
            relative_y >= (int64_t)window_outer_height(window->height, window->flags)) {
            continue;
        }
        inset = window_client_decorations(window->flags) ? 0U :
            compositor_corner_inset(
                (uint32_t)relative_y,
                window_outer_width(window->width, window->flags),
                window_outer_height(window->height, window->flags));
        if (relative_x >= (int64_t)inset &&
            relative_x < (int64_t)window_outer_width(window->width, window->flags) - inset) {
            return window;
        }
    }
    return 0;
}

/* 键盘没有窗口坐标可用于命中测试。若焦点暂时为空，仍把按键交给最上层
 * 可见窗口；单窗口 shell 不应因为一次焦点切换而丢失全部输入。 */
static window_server_window_t *keyboard_window_locked(void) {
    window_server_window_t *window =
        find_window_locked(g_window_server.focused_identifier);
    if (window != 0 &&
        !window->minimized &&
        (window->flags & OS_WINDOW_VISIBLE) != 0U) return window;
    for (uint32_t index = g_window_server.count; index != 0U; --index) {
        window = g_window_server.windows[index - 1U];
        if (window != 0 &&
            !window->minimized &&
            (window->flags & OS_WINDOW_VISIBLE) != 0U) {
            g_window_server.focused_identifier = window->identifier;
            return window;
        }
    }
    return 0;
}

static void focus_locked(window_server_window_t *window) {
    window_server_window_t *old_focused;
    uint32_t position = 0U;
    if (window == 0 ||
        window->minimized ||
        (window->flags & OS_WINDOW_VISIBLE) == 0U) {
        return;
    }
    old_focused = find_window_locked(g_window_server.focused_identifier);
    for (; position < g_window_server.count; ++position) {
        if (g_window_server.windows[position] == window) break;
    }
    /* Clicking the already-frontmost window does not change composition.
     * Avoid turning an otherwise tiny client damage update into a full-window
     * damage solely because focus was requested again. */
    if (old_focused == window && position + 1U >= g_window_server.count) {
        return;
    }
    if (position + 1U < g_window_server.count) {
        for (uint32_t index = position + 1U; index < g_window_server.count; ++index) {
            g_window_server.windows[index - 1U] = g_window_server.windows[index];
        }
        g_window_server.windows[g_window_server.count - 1U] = window;
    }
    g_window_server.focused_identifier = window->identifier;
    window_mark_window_locked(old_focused);
    window_mark_window_locked(window);
}


/*
 * Fast WB -> WB pixel copy.
 *
 * Source is the shared window backing page through the physical direct map.
 * Destination is the normal-RAM composite framebuffer.
 *
 * REP MOVSQ uses only integer registers and is well suited to contiguous
 * scanline spans.  No kernel SIMD/FPU state is touched.
 */
static inline void compositor_copy_wb_pixels(
    volatile uint32_t *destination,
    const uint32_t *source,
    uint32_t pixels) {

    uint64_t qwords;

    if (destination == 0 ||
        source == 0 ||
        pixels == 0U) {
        return;
    }

    qwords = pixels >> 1U;

    if (qwords != 0U) {
        void *out =
            (void *)(uintptr_t)destination;

        const void *in =
            (const void *)source;

        uint64_t count = qwords;

        __asm__ volatile (
            "rep movsq"
            : "+D"(out),
              "+S"(in),
              "+c"(count)
            :
            : "memory");
    }

    /*
     * REP MOVSQ copied an even number of pixels.
     */
    if ((pixels & 1U) != 0U) {
        destination[pixels - 1U] =
            source[pixels - 1U];
    }
}


/*
 * Fast background fill for a clipped surface span.
 *
 * This is mainly used during live resize or if a shared backing page cannot
 * be resolved.  Pack two XRGB pixels into one 64-bit store.
 */
static void compositor_fill_surface_pixels(
    volatile uint32_t *destination,
    uint32_t pixels,
    uint32_t color) {

    uint64_t pair;

    if (destination == 0 || pixels == 0U) {
        return;
    }

    /*
     * Align destination before using uint64_t stores.
     */
    if ((((uintptr_t)destination) & 7U) != 0U) {
        *destination++ = color;
        --pixels;
    }

    pair =
        (uint64_t)color |
        ((uint64_t)color << 32U);

    while (pixels >= 8U) {
        volatile uint64_t *out =
            (volatile uint64_t *)(void *)destination;

        out[0] = pair;
        out[1] = pair;
        out[2] = pair;
        out[3] = pair;

        destination += 8U;
        pixels -= 8U;
    }

    while (pixels >= 2U) {
        *(volatile uint64_t *)(void *)destination =
            pair;

        destination += 2U;
        pixels -= 2U;
    }

    if (pixels != 0U) {
        *destination = color;
    }
}


/*
 * Copy one horizontal window-surface span.
 *
 * The span is split only when it crosses a 4K VM_OBJECT_SHARED backing page.
 * Therefore page lookup happens roughly once per 1024 XRGB8888 pixels rather
 * than once per pixel.
 *
 * cached_page/cached_base persist across scanlines, so a page shared by the
 * tail of one row and the beginning of the next does not need to be resolved
 * twice.
 */

/*
 * Persistent direct-page table for a window surface.
 *
 * VM_OBJECT_SHARED pages do not move while their object remains alive.
 * The compositor snapshot holds a reference to the live window, therefore
 * positive direct mappings may safely be reused across compositor frames.
 *
 * Missing pages are NEVER cached.  If Ring3 faults a new page later, the next
 * compositor lookup will see an empty slot and resolve it normally.
 */
typedef struct compositor_surface_page_cache {
    shared_section_t *section;
    uint32_t page_count;
    uint32_t reserved;

    /*
     * Flexible array:
     *
     *     pages[page_index] -> kernel direct-map address
     *
     * NULL means "not resolved yet", not "page permanently absent".
     */
    uint8_t *pages[];
} compositor_surface_page_cache_t;


/*
 * Return/create the per-window direct-page table.
 *
 * Allocation failure is harmless: the compositor simply falls back to the
 * normal VM lookup path.
 */
static compositor_surface_page_cache_t *
compositor_surface_cache_get(
    const compositor_window_view_t *view) {

    window_server_window_t *reference;
    compositor_surface_page_cache_t *cache;
    uint64_t page_count64;
    size_t bytes;

    if (view == 0 ||
        view->reference == 0 ||
        view->section == 0 ||
        view->buffer_size == 0U) {
        return 0;
    }

    reference = view->reference;

    /*
     * Existing window section means the direct-page table remains valid.
     * Avoid recomputing page_count on the normal path.
     */
    cache =
        (compositor_surface_page_cache_t *)
            reference->compositor_cache;

    if (cache != 0 &&
        cache->section == view->section) {
        return cache;
    }

    page_count64 =
        (view->buffer_size + PAGE_SIZE - 1U) >>
        PAGE_SHIFT;

    if (page_count64 == 0U ||
        page_count64 > UINT32_MAX) {
        return 0;
    }

    /*
     * The current implementation keeps a stable section for a window.
     * Still validate it here so a future section replacement cannot expose
     * stale direct pointers.
     */
    if (cache != 0 &&
        cache->section == view->section &&
        cache->page_count == (uint32_t)page_count64) {
        return cache;
    }

    if (cache != 0) {
        reference->compositor_cache = 0;
        kfree(cache);
        cache = 0;
    }

    /*
     * page_count is bounded to uint32_t and the kernel is 64-bit, so this
     * multiplication is safely representable as size_t.
     */
    bytes =
        sizeof(compositor_surface_page_cache_t) +
        (size_t)page_count64 * sizeof(uint8_t *);

    cache =
        (compositor_surface_page_cache_t *)
            kzalloc(bytes, 0);

    if (cache == 0) {
        return 0;
    }

    cache->section =
        view->section;

    cache->page_count =
        (uint32_t)page_count64;

    cache->reserved = 0U;

    reference->compositor_cache =
        cache;

    return cache;
}


/*
 * Cached wrapper around vm_object_shared_page_direct().
 *
 * Fast hit:
 *
 *     page_index -> pointer array -> direct address
 *
 * No VM lock, no backing-page list traversal.
 */

/*
 * Sequential surface rendering normally crosses shared pages in ascending
 * order.  Once the next page already has a positive direct-page cache entry,
 * pull its first cache line toward L1 before the current page is exhausted.
 *
 * This is only a performance hint. Missing pages are never resolved or
 * allocated here.
 */
static inline void compositor_prefetch_surface_page(
    const compositor_surface_page_cache_t *cache,
    uint64_t page_index) {

    uint8_t *next;

    if (cache == 0 ||
        page_index + 1U >= cache->page_count) {
        return;
    }

    next =
        cache->pages[page_index + 1U];

    if (next == 0) {
        return;
    }

    __asm__ volatile (
        "prefetcht0 (%0)"
        :
        : "r"(next)
        : "memory");
}


static inline kstatus_t compositor_surface_page_direct(
    const compositor_window_view_t *view,
    vm_object_t *object,
    uint64_t offset,
    bool create,
    uint8_t **out) {

    window_server_window_t *reference;
    compositor_surface_page_cache_t *cache;
    uint64_t page_index;
    uint8_t *base;
    kstatus_t status;

    if (view == 0 ||
        object == 0 ||
        out == 0) {
        return K_EINVAL;
    }

    page_index =
        offset >> PAGE_SHIFT;

    reference =
        view->reference;

    /*
     * Ultra-hot path.
     *
     * Once the per-window cache exists, do NOT call
     * compositor_surface_cache_get() on every surface page.
     *
     * Normal repaint path becomes:
     *
     *     window
     *       -> cache
     *       -> pages[index]
     *       -> direct address
     */
    cache =
        reference != 0 ?
        (compositor_surface_page_cache_t *)
            reference->compositor_cache :
        0;

    if (cache != 0 &&
        cache->section == view->section &&
        page_index < cache->page_count) {

        base =
            cache->pages[page_index];

        if (base != 0) {
            compositor_prefetch_surface_page(
                cache,
                page_index);

            *out = base;
            return K_OK;
        }
    } else {
        /*
         * Slow initialization/stale-cache path.
         *
         * This normally runs only before the first positive page mapping for
         * the window, or if a future implementation replaces its section.
         */
        cache =
            compositor_surface_cache_get(view);

        if (cache != 0 &&
            page_index < cache->page_count) {

            base =
                cache->pages[page_index];

            if (base != 0) {
                *out = base;
                return K_OK;
            }
        }
    }

    /*
     * Cache miss.
     *
     * With create=false this is a pure lookup: the compositor still never
     * allocates an untouched shared-surface page merely because it reads it.
     */
    status =
        vm_object_shared_page_direct(
            object,
            offset,
            create,
            out);

    if (status != K_OK) {
        return status;
    }

    /*
     * Positive mappings are stable for the lifetime of the shared section.
     * Cache only successful mappings. A missing page remains NULL so Ring3
     * can fault it in later and a subsequent compositor pass can discover it.
     */
    if (cache == 0) {
        cache =
            compositor_surface_cache_get(view);
    }

    if (cache != 0 &&
        page_index < cache->page_count &&
        *out != 0) {

        cache->pages[page_index] =
            *out;

        compositor_prefetch_surface_page(
            cache,
            page_index);
    }

    return K_OK;
}


static void compositor_copy_surface_span(
    const compositor_window_view_t *window,
    uint32_t row,
    uint32_t first_column,
    uint32_t last_column,
    volatile uint32_t *destination,
    uint64_t *cached_page,
    uint8_t **cached_base) {

    vm_object_t *object;
    uint64_t pixel_index;
    uint64_t source_offset;
    uint32_t remaining;

    if (window == 0 ||
        destination == 0 ||
        cached_page == 0 ||
        cached_base == 0 ||
        first_column >= last_column) {
        return;
    }

    remaining =
        last_column - first_column;

    /*
     * During live resize the client has not acknowledged the new stride yet.
     * Never interpret the old surface with the new dimensions.
     */
    if (window->resize_pending ||
        window->section == 0 ||
        window->section->vm_object == 0 ||
        window->owner_address == 0U) {

        compositor_fill_surface_pixels(
            destination,
            remaining,
            window->background);

        return;
    }

    object =
        window->section->vm_object;

    if (object->type != VM_OBJECT_SHARED) {
        compositor_fill_surface_pixels(
            destination,
            remaining,
            window->background);

        return;
    }

    pixel_index =
        (uint64_t)row *
            window->width +
        first_column;

    if (pixel_index >
        UINT64_MAX / sizeof(uint32_t)) {

        compositor_fill_surface_pixels(
            destination,
            remaining,
            window->background);

        return;
    }

    source_offset =
        pixel_index *
        sizeof(uint32_t);

    /*
     * Snapshot dimensions should always fit the reserved surface capacity.
     * Keep the bounds check here because compositor code must never read past
     * the shared object if a malformed window state slips through.
     */
    if (source_offset >= window->buffer_size) {
        compositor_fill_surface_pixels(
            destination,
            remaining,
            window->background);

        return;
    }

    {
        uint64_t available_bytes =
            window->buffer_size -
            source_offset;

        uint64_t requested_bytes =
            (uint64_t)remaining *
            sizeof(uint32_t);

        if (requested_bytes >
            available_bytes) {

            uint32_t available_pixels =
                (uint32_t)(
                    available_bytes /
                    sizeof(uint32_t));

            if (available_pixels < remaining) {
                compositor_fill_surface_pixels(
                    destination +
                        available_pixels,
                    remaining -
                        available_pixels,
                    window->background);

                remaining =
                    available_pixels;
            }
        }
    }

    while (remaining != 0U) {
        uint64_t page =
            source_offset &
            ~(uint64_t)(PAGE_SIZE - 1U);

        uint32_t in_page =
            (uint32_t)(
                source_offset &
                (PAGE_SIZE - 1U));

        uint32_t page_pixels =
            (PAGE_SIZE - in_page) /
            sizeof(uint32_t);

        uint32_t chunk =
            remaining < page_pixels ?
            remaining :
            page_pixels;

        /*
         * Keep a one-page hot cache across the entire window.  Linear XRGB
         * surfaces normally touch physical backing pages sequentially.
         */
        if (*cached_page != page) {
            uint8_t *base = 0;

            if (compositor_surface_page_direct(window,
                    object,
                    page,
                    true,
                    &base) == K_OK) {

                *cached_base = base;
            } else {
                *cached_base = 0;
            }

            *cached_page = page;
        }

        if (*cached_base != 0) {
            const uint32_t *source =
                (const uint32_t *)(const void *)(
                    *cached_base +
                    in_page);

            compositor_copy_wb_pixels(
                destination,
                source,
                chunk);
        } else {
            compositor_fill_surface_pixels(
                destination,
                chunk,
                window->background);
        }

        destination += chunk;

        source_offset +=
            (uint64_t)chunk *
            sizeof(uint32_t);

        remaining -= chunk;
    }
}



static void compositor_surface_locked(
    const compositor_window_view_t *window) {

    uint64_t cached_page = UINT64_MAX;
    uint8_t *cached_base = 0;

    int64_t frame_width;
    int64_t frame_height;
    int64_t surface_x;
    int64_t surface_y;

    if (window == 0) {
        return;
    }

    frame_width =
        (int64_t)window->width +
        window_frame_extra(window->flags);

    frame_height =
        (int64_t)window->height +
        window_frame_extra(window->flags) +
        window_titlebar_height(window->flags);

    surface_x =
        (int64_t)window->x +
        window_frame_border(window->flags);

    surface_y =
        (int64_t)window->y +
        window_client_offset_y(window->flags);

    for (uint32_t row = 0U;
         row < window->height;
         ++row) {

        int64_t destination_y =
            surface_y + row;

        int64_t frame_row =
            destination_y -
            window->y;

        int64_t first_column = 0;
        int64_t last_column =
            window->width;

        uint32_t inset;

        volatile uint32_t *destination;

        if (destination_y <
                (int64_t)
                    g_compositor_snapshot.damage_top ||
            destination_y >=
                (int64_t)
                    g_compositor_snapshot.damage_bottom) {
            continue;
        }

        inset = window_client_decorations(window->flags) ? 0U :
            (frame_row >= 0 && frame_row < frame_height ?
             compositor_corner_inset(
                 (uint32_t)frame_row,
                 (uint32_t)frame_width,
                 (uint32_t)frame_height) :
             0U);

        /*
         * Clip against the display.
         */
        if (surface_x + first_column < 0) {
            first_column =
                -surface_x;
        }

        if (surface_x + last_column >
            (int64_t)
                g_window_server.display_width) {

            last_column =
                (int64_t)
                    g_window_server.display_width -
                surface_x;
        }

        /*
         * Clip against current damage.
         */
        if (surface_x + first_column <
            (int64_t)
                g_compositor_snapshot.damage_left) {

            first_column =
                (int64_t)
                    g_compositor_snapshot.damage_left -
                surface_x;
        }

        if (surface_x + last_column >
            (int64_t)
                g_compositor_snapshot.damage_right) {

            last_column =
                (int64_t)
                    g_compositor_snapshot.damage_right -
                surface_x;
        }

        /*
         * Preserve the one-pixel decoration border around rounded windows.
         */
        if (first_column <
            (int64_t)inset) {

            first_column =
                inset;
        }

        if (last_column >
            (int64_t)window->width -
                inset) {

            last_column =
                (int64_t)window->width -
                inset;
        }

        if (first_column < 0) {
            first_column = 0;
        }

        if (last_column >
            (int64_t)window->width) {

            last_column =
                window->width;
        }

        if (first_column >= last_column) {
            continue;
        }

        destination =
            g_window_server.composite_framebuffer +

            (uint64_t)
                (uint32_t)destination_y *
                g_window_server.display_stride +

            (uint32_t)(
                surface_x +
                first_column);

        compositor_copy_surface_span(
            window,
            row,
            (uint32_t)first_column,
            (uint32_t)last_column,
            destination,
            &cached_page,
            &cached_base);
    }
}


static bool compositor_window_intersects_damage_locked(
    const compositor_window_view_t *window) {
    int64_t left;
    int64_t top;
    int64_t right;
    int64_t bottom;
    if (window == 0) return false;
    left = window->x;
    top = window->y;
    right = left + (int64_t)window_outer_width(window->width, window->flags);
    bottom = top + (int64_t)window_outer_height(window->height, window->flags);
    return left < (int64_t)g_compositor_snapshot.damage_right &&
           right > (int64_t)g_compositor_snapshot.damage_left &&
           top < (int64_t)g_compositor_snapshot.damage_bottom &&
           bottom > (int64_t)g_compositor_snapshot.damage_top;
}

/* Linux Xcursor/Adwaita top_left_arrow, 24x24, hotspot (3,1).
 * The source is an ARGB cursor surface; it is composited pixel by pixel
 * with the same source-over rule as a Linux software cursor. */
static const uint32_t g_linux_cursor_argb[WINDOW_CURSOR_WIDTH * WINDOW_CURSOR_HEIGHT] = {
    0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x00000000U, 0x02000000U, 0x45414141U, 0x03000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x01000000U, 0x09000000U, 0xF7F5F5F5U, 0x4B414141U, 0x04000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x02000000U, 0x15000000U, 0xFFFFFFFFU, 0xF7EAEAEAU, 0x4E414141U, 0x04000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x03000000U, 0x1A000000U, 0xFFFFFFFFU, 0xFF3C3C3CU, 0xF7E9E9E9U, 0x50434343U, 0x04000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x03000000U, 0x1B000000U, 0xFFFFFFFFU, 0xFF000000U, 0xFF3C3C3CU, 0xF7E9E9E9U, 0x50434343U, 0x04000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x03000000U, 0x1B000000U, 0xFFFFFFFFU, 0xFF000000U, 0xFF000000U, 0xFF3C3C3CU, 0xF7E9E9E9U, 0x50434343U, 0x04000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x03000000U, 0x1B000000U, 0xFFFFFFFFU, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF3C3C3CU, 0xF7E9E9E9U, 0x50434343U, 0x04000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x03000000U, 0x1B000000U, 0xFFFFFFFFU, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF3C3C3CU, 0xF7E9E9E9U, 0x50434343U, 0x04000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x03000000U, 0x1B000000U, 0xFFFFFFFFU, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF3C3C3CU, 0xF7E9E9E9U, 0x50434343U, 0x04000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x03000000U, 0x1B000000U, 0xFFFFFFFFU, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF3C3C3CU, 0xF7E9E9E9U, 0x50434343U, 0x04000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x03000000U, 0x1B000000U, 0xFFFFFFFFU, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF3C3C3CU, 0xF7E9E9E9U, 0x50434343U, 0x04000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x03000000U, 0x1B000000U, 0xFFFFFFFFU, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF3C3C3CU, 0xF7E9E9E9U, 0x50434343U, 0x04000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x03000000U, 0x1B000000U, 0xFFFFFFFFU, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFF000000U, 0xFDC1C1C1U, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xF8F5F5F5U, 0x4C414141U, 0x03000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x03000000U, 0x1B000000U, 0xFFFFFFFFU, 0xFF000000U, 0xFF000000U, 0xFF414141U, 0xFF404040U, 0xFF000000U, 0xFF4D4D4DU, 0xE8CBCBCBU, 0x5F000000U, 0x59000000U, 0x54000000U, 0x3F000000U, 0x1A000000U, 0x05000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x03000000U, 0x1B000000U, 0xFFFFFFFFU, 0xFF000000U, 0xFF414141U, 0xFCF3F3F3U, 0xF7BABABAU, 0xFF000000U, 0xFF010101U, 0xF7D6D6D6U, 0x65414141U, 0x1E000000U, 0x1A000000U, 0x15000000U, 0x09000000U, 0x03000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x03000000U, 0x1B000000U, 0xFFFFFFFFU, 0xFF3D3D3DU, 0xF9E9E9E9U, 0x90464646U, 0xF4E1E1E1U, 0xFF323232U, 0xFF000000U, 0xFE626262U, 0xCDB5B5B5U, 0x0E000000U, 0x04000000U, 0x02000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x03000000U, 0x1B000000U, 0xFFFFFFFFU, 0xF9EAEAEAU, 0x82404040U, 0x43000000U, 0xA5737373U, 0xF9A5A5A5U, 0xFF000000U, 0xFF070707U, 0xF8E5E5E5U, 0x3E2C2C2CU, 0x03000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x03000000U, 0x1A000000U, 0xF9F4F4F4U, 0x7F404040U, 0x30000000U, 0x1E000000U, 0x460E0E0EU, 0xF8EBEBEBU, 0xFF1F1F1FU, 0xFF000000U, 0xFD787878U, 0xB79F9F9FU, 0x09000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x02000000U, 0x15000000U, 0x6F404040U, 0x2D000000U, 0x11000000U, 0x0A000000U, 0x21000000U, 0xB4898989U, 0xFB8E8E8EU, 0xFF000000U, 0xFF101010U, 0xF9F0F0F0U, 0x19070707U, 0x02000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x01000000U, 0x09000000U, 0x19000000U, 0x0D000000U, 0x04000000U, 0x03000000U, 0x11000000U, 0x521A1A1AU, 0xF9ECECECU, 0xFF1F1F1FU, 0xFF202020U, 0xFAF0F0F0U, 0x1E060606U, 0x03000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x00000000U, 0x02000000U, 0x05000000U, 0x03000000U, 0x01000000U, 0x01000000U, 0x07000000U, 0x23000000U, 0x99636363U, 0xFAF0F0F0U, 0xFAF0F0F0U, 0x96626262U, 0x17000000U, 0x02000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x02000000U, 0x0F000000U, 0x31000000U, 0x53050505U, 0x52050505U, 0x31000000U, 0x0E000000U, 0x01000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x01000000U, 0x04000000U, 0x0E000000U, 0x17000000U, 0x17000000U, 0x0E000000U, 0x04000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U
};

static void compositor_cursor_locked(void) {
    int64_t origin_x;
    int64_t origin_y;

    int64_t cursor_clip_left;
    int64_t cursor_clip_top;
    int64_t cursor_clip_right;
    int64_t cursor_clip_bottom;

    if (g_window_server.composite_framebuffer == 0 ||
        g_window_server.display_width == 0U ||
        g_window_server.display_height == 0U) {
        return;
    }

    origin_x =
        (int64_t)g_compositor_snapshot.pointer_x -
        WINDOW_CURSOR_HOTSPOT_X;

    origin_y =
        (int64_t)g_compositor_snapshot.pointer_y -
        WINDOW_CURSOR_HOTSPOT_Y;

    /*
     * Clip once:
     *
     *     cursor rectangle
     *       ∩ screen
     *       ∩ current damage
     *
     * The old implementation visited all 24x24 source pixels and repeated
     * these bounds checks inside compositor_blend_pixel_locked().
     */
    cursor_clip_left = origin_x;
    cursor_clip_top = origin_y;

    cursor_clip_right =
        origin_x + WINDOW_CURSOR_WIDTH;

    cursor_clip_bottom =
        origin_y + WINDOW_CURSOR_HEIGHT;

    if (cursor_clip_left < 0) {
        cursor_clip_left = 0;
    }

    if (cursor_clip_top < 0) {
        cursor_clip_top = 0;
    }

    if (cursor_clip_right >
        (int64_t)g_window_server.display_width) {
        cursor_clip_right =
            g_window_server.display_width;
    }

    if (cursor_clip_bottom >
        (int64_t)g_window_server.display_height) {
        cursor_clip_bottom =
            g_window_server.display_height;
    }

    if (cursor_clip_left <
        (int64_t)g_compositor_snapshot.damage_left) {
        cursor_clip_left =
            g_compositor_snapshot.damage_left;
    }

    if (cursor_clip_top <
        (int64_t)g_compositor_snapshot.damage_top) {
        cursor_clip_top =
            g_compositor_snapshot.damage_top;
    }

    if (cursor_clip_right >
        (int64_t)g_compositor_snapshot.damage_right) {
        cursor_clip_right =
            g_compositor_snapshot.damage_right;
    }

    if (cursor_clip_bottom >
        (int64_t)g_compositor_snapshot.damage_bottom) {
        cursor_clip_bottom =
            g_compositor_snapshot.damage_bottom;
    }

    if (cursor_clip_left >= cursor_clip_right ||
        cursor_clip_top >= cursor_clip_bottom) {
        return;
    }

    for (int64_t y = cursor_clip_top;
         y < cursor_clip_bottom;
         ++y) {

        uint32_t source_row =
            (uint32_t)(y - origin_y);

        uint32_t source_column =
            (uint32_t)(cursor_clip_left - origin_x);

        volatile uint32_t *destination =
            g_window_server.composite_framebuffer +
            (uint64_t)y *
                g_window_server.display_stride +
            (uint32_t)cursor_clip_left;

        const uint32_t *source =
            &g_linux_cursor_argb[
                source_row * WINDOW_CURSOR_WIDTH +
                source_column];

        uint32_t pixels =
            (uint32_t)(
                cursor_clip_right -
                cursor_clip_left);

        for (uint32_t column = 0U;
             column < pixels;
             ++column) {

            uint32_t pixel =
                source[column];

            uint32_t alpha =
                pixel >> 24;

            if (alpha == 0U) {
                ++destination;
                continue;
            }

            uint32_t color =
                pixel & 0x00FFFFFFU;

            /*
             * Most solid cursor pixels take this cheap path.
             */
            if (alpha >= 255U) {
                *destination++ = color;
                continue;
            }

            uint32_t current =
                *destination;

            uint32_t inverse =
                255U - alpha;

            uint32_t red =
                ((((current >> 16) & 0xFFU) *
                      inverse) +
                 (((color >> 16) & 0xFFU) *
                      alpha) +
                 127U) /
                255U;

            uint32_t green =
                ((((current >> 8) & 0xFFU) *
                      inverse) +
                 (((color >> 8) & 0xFFU) *
                      alpha) +
                 127U) /
                255U;

            uint32_t blue =
                (((current & 0xFFU) *
                      inverse) +
                 ((color & 0xFFU) *
                      alpha) +
                 127U) /
                255U;

            *destination++ =
                (red << 16) |
                (green << 8) |
                blue;
        }
    }
}



/*
 * Return true only when the COMPLETE current damage rectangle lies inside an
 * unquestionably opaque part of this window.
 *
 * Do not attempt precise rounded-corner subtraction here.  Instead remove
 * WINDOW_CORNER_RADIUS pixels from every side and use only the central
 * rectangle.  This is deliberately conservative:
 *
 *     +---------------- window ----------------+
 *     | rounded / uncertain edge               |
 *     |    +-------- guaranteed opaque -----+  |
 *     |    |                                |  |
 *     |    +--------------------------------+  |
 *     +-----------------------------------------+
 *
 * A false result only loses an optimization; it never affects correctness.
 */

/*
 * Return true only when the complete current damage rectangle lies in the
 * guaranteed opaque interior of the client surface.
 *
 * In that case drawing the window frame/background first is redundant:
 * compositor_surface_locked() will overwrite every damaged pixel.
 *
 * Keep a conservative WINDOW_CORNER_RADIUS margin so rounded corners and the
 * one-pixel frame can never enter this fast path.
 */
static inline bool compositor_damage_inside_surface_interior(
    const compositor_window_view_t *window) {

    int64_t left;
    int64_t top;
    int64_t right;
    int64_t bottom;

    if (window == 0 ||
        window->width <= WINDOW_CORNER_RADIUS * 2U ||
        window->height <= WINDOW_CORNER_RADIUS * 2U) {
        return false;
    }

    left =
        (int64_t)window->x +
        window_frame_border(window->flags) +
        WINDOW_CORNER_RADIUS;

    top =
        (int64_t)window->y +
        window_client_offset_y(window->flags) +
        WINDOW_CORNER_RADIUS;

    right =
        (int64_t)window->x +
        window_frame_border(window->flags) +
        (int64_t)window->width -
        WINDOW_CORNER_RADIUS;

    bottom =
        (int64_t)window->y +
        window_client_offset_y(window->flags) +
        (int64_t)window->height -
        WINDOW_CORNER_RADIUS;

    return
        (int64_t)g_compositor_snapshot.damage_left >= left &&
        (int64_t)g_compositor_snapshot.damage_top >= top &&
        (int64_t)g_compositor_snapshot.damage_right <= right &&
        (int64_t)g_compositor_snapshot.damage_bottom <= bottom;
}


static bool compositor_window_fully_covers_damage(
    const compositor_window_view_t *window) {

    int64_t frame_left;
    int64_t frame_top;
    int64_t frame_right;
    int64_t frame_bottom;

    int64_t opaque_left;
    int64_t opaque_top;
    int64_t opaque_right;
    int64_t opaque_bottom;

    uint64_t frame_width;
    uint64_t frame_height;

    if (window == 0 ||
        (window->flags & OS_WINDOW_VISIBLE) == 0U) {
        return false;
    }

    frame_width =
        (uint64_t)window->width +
        window_frame_extra(window->flags);

    frame_height =
        (uint64_t)window->height + window_frame_extra(window->flags) +
        window_titlebar_height(window->flags);

    /*
     * Tiny windows do not have a useful conservative interior.
     */
    if (frame_width <=
            WINDOW_CORNER_RADIUS * 2U ||
        frame_height <=
            WINDOW_CORNER_RADIUS * 2U) {
        return false;
    }

    frame_left = window->x;
    frame_top = window->y;

    frame_right =
        frame_left +
        (int64_t)frame_width;

    frame_bottom =
        frame_top +
        (int64_t)frame_height;

    opaque_left =
        frame_left +
        WINDOW_CORNER_RADIUS;

    opaque_top =
        frame_top +
        WINDOW_CORNER_RADIUS;

    opaque_right =
        frame_right -
        WINDOW_CORNER_RADIUS;

    opaque_bottom =
        frame_bottom -
        WINDOW_CORNER_RADIUS;

    return
        (int64_t)g_compositor_snapshot.damage_left >=
            opaque_left &&

        (int64_t)g_compositor_snapshot.damage_top >=
            opaque_top &&

        (int64_t)g_compositor_snapshot.damage_right <=
            opaque_right &&

        (int64_t)g_compositor_snapshot.damage_bottom <=
            opaque_bottom;
}


/*
 * Find the highest Z-order window that completely hides everything below it
 * for the current damage rectangle.
 *
 * Windows in the snapshot are bottom -> top, so scan backwards.
 *
 * UINT32_MAX means the desktop is still required.
 */
static uint32_t compositor_topmost_damage_cover(void) {

    uint32_t count =
        g_compositor_snapshot.window_count;

    while (count != 0U) {
        uint32_t index =
            count - 1U;

        const compositor_window_view_t *window =
            &g_compositor_snapshot.windows[index];

        if (compositor_window_fully_covers_damage(
                window)) {
            return index;
        }

        count = index;
    }

    return UINT32_MAX;
}




/*
 * Return the screen-space rectangle of one titlebar button.
 *
 * slot from right:
 *
 *     close      = 0
 *     maximize   = 1
 *     minimize   = 2
 */
static bool window_title_button_rect(
    int32_t window_x,
    int32_t window_y,
    uint32_t window_width,
    uint32_t button,
    int32_t *out_x,
    int32_t *out_y) {

    uint32_t slot;
    int64_t x;

    if (out_x == 0 ||
        out_y == 0 ||
        window_width <
            WINDOW_TITLE_CONTROLS_WIDTH) {
        return false;
    }

    switch (button) {
        case WINDOW_TITLE_BUTTON_CLOSE:
            slot = 0U;
            break;

        case WINDOW_TITLE_BUTTON_MAXIMIZE:
            slot = 1U;
            break;

        case WINDOW_TITLE_BUTTON_MINIMIZE:
            slot = 2U;
            break;

        default:
            return false;
    }

    x =
        (int64_t)window_x +
        WINDOW_FRAME_BORDER +
        window_width -
        WINDOW_TITLE_BUTTON_RIGHT_MARGIN -
        WINDOW_TITLE_BUTTON_SIZE -
        (uint64_t)slot *
            (WINDOW_TITLE_BUTTON_SIZE +
             WINDOW_TITLE_BUTTON_GAP);

    if (x < -2147483648LL ||
        x > 2147483647LL) {
        return false;
    }

    *out_x =
        (int32_t)x;

    *out_y =
        window_y +
        (int32_t)WINDOW_FRAME_BORDER +
        (int32_t)(
            (WINDOW_TITLEBAR_HEIGHT -
             WINDOW_TITLE_BUTTON_SIZE) /
            2U);

    return true;
}


/*
 * Ring0 decoration hit testing.
 *
 * A title-control hit is consumed by the window server and must never become
 * client input or start a titlebar drag.
 */
static uint32_t window_title_button_at_locked(
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
         index <
             sizeof(buttons) /
             sizeof(buttons[0]);
         ++index) {

        int32_t x;
        int32_t y;

        if (!window_title_button_rect(
                window->x,
                window->y,
                window->width,
                buttons[index],
                &x,
                &y)) {
            continue;
        }

        if ((int64_t)pointer_x >= x &&
            (int64_t)pointer_y >= y &&
            (int64_t)pointer_x <
                (int64_t)x +
                WINDOW_TITLE_BUTTON_SIZE &&
            (int64_t)pointer_y <
                (int64_t)y +
                WINDOW_TITLE_BUTTON_SIZE) {

            return buttons[index];
        }
    }

    return WINDOW_TITLE_BUTTON_NONE;
}


/*
 * Flat controls: no shadow, no extra framebuffer and no alpha surface.
 */
static void compositor_title_controls_locked(
    const compositor_window_view_t *window) {

    static const uint32_t buttons[] = {
        WINDOW_TITLE_BUTTON_MINIMIZE,
        WINDOW_TITLE_BUTTON_MAXIMIZE,
        WINDOW_TITLE_BUTTON_CLOSE,
    };

    bool focused;

    if (window == 0) {
        return;
    }

    if (window_client_decorations(window->flags)) {
        return;
    }

    focused =
        window->identifier ==
        g_compositor_snapshot.focused_identifier;

    for (uint32_t index = 0U;
         index <
             sizeof(buttons) /
             sizeof(buttons[0]);
         ++index) {

        uint32_t button =
            buttons[index];

        int32_t x;
        int32_t y;

        uint32_t background;
        uint32_t glyph;

        if (!window_title_button_rect(
                window->x,
                window->y,
                window->width,
                button,
                &x,
                &y)) {
            continue;
        }

        background =
            focused ?
                0x001E2A36U :
                0x00182028U;

        glyph =
            focused ?
                0x00B7C3D0U :
                0x007F8B96U;

        /*
         * Keep close visually identifiable without a permanent bright-red
         * Windows-style rectangle.
         */
        if (button ==
            WINDOW_TITLE_BUTTON_CLOSE) {

            background =
                focused ?
                    0x002C2228U :
                    0x00241C20U;

            glyph =
                focused ?
                    0x00E28A92U :
                    0x009B6870U;
        }

        compositor_fill_rounded_locked(
            x,
            y,
            WINDOW_TITLE_BUTTON_SIZE,
            WINDOW_TITLE_BUTTON_SIZE,
            WINDOW_CORNER_RADIUS,
            background);

        if (button ==
            WINDOW_TITLE_BUTTON_MINIMIZE) {

            compositor_fill_locked(
                x + 6,
                y + 11,
                8U,
                1U,
                glyph);

        } else if (
            button ==
            WINDOW_TITLE_BUTTON_MAXIMIZE) {

            if (!window->maximized) {
                /*
                 * Normal maximize glyph.
                 */
                compositor_fill_locked(
                    x + 6,
                    y + 6,
                    8U,
                    1U,
                    glyph);

                compositor_fill_locked(
                    x + 6,
                    y + 13,
                    8U,
                    1U,
                    glyph);

                compositor_fill_locked(
                    x + 6,
                    y + 6,
                    1U,
                    8U,
                    glyph);

                compositor_fill_locked(
                    x + 13,
                    y + 6,
                    1U,
                    8U,
                    glyph);

            } else {
                /*
                 * Restore glyph: two overlapping rectangles.
                 */
                compositor_fill_locked(
                    x + 8,
                    y + 6,
                    7U,
                    1U,
                    glyph);

                compositor_fill_locked(
                    x + 14,
                    y + 6,
                    1U,
                    7U,
                    glyph);

                compositor_fill_locked(
                    x + 7,
                    y + 8,
                    7U,
                    1U,
                    glyph);

                compositor_fill_locked(
                    x + 7,
                    y + 8,
                    1U,
                    7U,
                    glyph);

                compositor_fill_locked(
                    x + 7,
                    y + 14,
                    7U,
                    1U,
                    glyph);

                compositor_fill_locked(
                    x + 13,
                    y + 8,
                    1U,
                    7U,
                    glyph);
            }
        } else if (
            button ==
            WINDOW_TITLE_BUTTON_CLOSE) {

            /*
             * Small X.
             */
            for (uint32_t step = 0U;
                 step < 7U;
                 ++step) {

                compositor_fill_locked(
                    x + 7 +
                        (int32_t)step,
                    y + 7 +
                        (int32_t)step,
                    1U,
                    1U,
                    glyph);

                compositor_fill_locked(
                    x + 13 -
                        (int32_t)step,
                    y + 7 +
                        (int32_t)step,
                    1U,
                    1U,
                    glyph);
            }
        }
    }
}


static void compositor_titlebar_locked(
    const compositor_window_view_t *window,
    uint32_t frame_color) {

    bool focused;

    uint32_t title_color;
    uint32_t separator_color;
    uint32_t text_color;

    uint32_t outer_width;
    uint32_t outer_height;

    uint32_t max_chars;

    int32_t text_x;
    int32_t text_y;

    if (window == 0) {
        return;
    }

    if (window_client_decorations(window->flags)) {
        return;
    }

    focused =
        window->identifier ==
        g_compositor_snapshot.focused_identifier;

    title_color =
        focused ?
            0x00172230U :
            0x00131921U;

    separator_color =
        focused ?
            0x0030475FU :
            0x00232D37U;

    text_color =
        focused ?
            0x00ECF3FAU :
            0x009AA8B7U;

    outer_width =
        window->width +
        window_frame_extra(window->flags);

    outer_height =
        window->height +
        window_frame_extra(window->flags) +
        window_titlebar_height(window->flags);

    /*
     * Complete flat outer frame.
     */
    compositor_fill_rounded_locked(
        window->x,
        window->y,
        outer_width,
        outer_height,
        WINDOW_CORNER_RADIUS,
        frame_color);

    /*
     * Keep the topmost rounded corner pixels owned by the outer frame.
     */
    if (window->width >
        WINDOW_CORNER_RADIUS * 2U) {

        compositor_fill_locked(
            window->x +
                (int32_t)WINDOW_CORNER_RADIUS,
            window->y +
                (int32_t)window_frame_border(window->flags),
            window->width -
                WINDOW_CORNER_RADIUS * 2U,
            window_titlebar_height(window->flags),
            title_color);
    }

    /*
     * Fill the central/lower titlebar portion.
     */
    if (window_titlebar_height(window->flags) >
        WINDOW_CORNER_RADIUS) {

        compositor_fill_locked(
            window->x +
                (int32_t)window_frame_border(window->flags),
            window->y +
                (int32_t)WINDOW_CORNER_RADIUS,
            window->width,
            window_titlebar_height(window->flags) -
                WINDOW_CORNER_RADIUS +
                window_frame_border(window->flags),
            title_color);
    }

    /*
     * Client/titlebar separator.
     */
    compositor_fill_locked(
        window->x +
            (int32_t)window_frame_border(window->flags),
        window->y +
            (int32_t)window_client_offset_y(window->flags) -
            1,
        window->width,
        1U,
        separator_color);

    compositor_title_controls_locked(
        window);

    /*
     * Window title using the existing 8x16 kernel font.
     * Keep it out of the control-button area.
     */
    if (window->width <=
        WINDOW_TITLE_CONTROLS_WIDTH + 24U) {
        return;
    }

    max_chars =
        (window->width -
         WINDOW_TITLE_CONTROLS_WIDTH -
         24U) /
        8U;

    if (max_chars > 31U) {
        max_chars = 31U;
    }

    text_x =
        window->x + 12;

    text_y =
        window->y +
        (int32_t)window_frame_border(window->flags) +
        (int32_t)(
            (window_titlebar_height(window->flags) - 16U) /
            2U);

    for (uint32_t index = 0U;
         index < max_chars &&
         window->title[index] != '\0';
         ++index) {

        desktop_draw_small_glyph_locked(
            text_x +
                (int32_t)(index * 8U),
            text_y,
            window->title[index],
            text_color);
    }
}


static void compositor_region_locked(void) {
    uint32_t damage_left =
        g_compositor_snapshot.damage_left;

    uint32_t damage_top =
        g_compositor_snapshot.damage_top;

    uint32_t damage_right =
        g_compositor_snapshot.damage_right;

    uint32_t damage_bottom =
        g_compositor_snapshot.damage_bottom;

    if (damage_left >= damage_right ||
        damage_top >= damage_bottom) {
        return;
    }

    /*
     * Desktop rendering is now also based on snapshot damage, pointer and
     * hover state.  It does not consume mutable scene state.
     */
    uint32_t first_window =
        compositor_topmost_damage_cover();

    if (first_window == UINT32_MAX) {
        /*
         * No window completely hides this damage; rebuild the desktop and
         * compose from the bottom as usual.
         */
        desktop_draw_wallpaper_locked();
        first_window = 0U;
    }


    for (uint32_t index = first_window; index < g_compositor_snapshot.window_count; ++index) {

        const compositor_window_view_t *window =
            &g_compositor_snapshot.windows[index];

        uint32_t frame_color;

        if ((window->flags & OS_WINDOW_VISIBLE) == 0U ||
            !compositor_window_intersects_damage_locked(window)) {
            continue;
        }

        frame_color =
            window->identifier ==
            g_compositor_snapshot.focused_identifier ?
                0x005F8FC4U :
                0x0028323EU;

        if (!window_client_decorations(window->flags) &&
            !compositor_damage_inside_surface_interior(window)) {
            compositor_titlebar_locked(window, frame_color);
        }

        compositor_surface_locked(window);
    }

    compositor_cursor_locked();

    /*
     * Do not expose this region to the scanout yet.
     *
     * All dirty regions are first completed in composite_framebuffer.
     * compositor_commit_snapshot() publishes the entire frame transaction
     * after every region is ready.
     */
}

static bool compositor_snapshot_begin_locked(void) {
    compositor_snapshot_t *snapshot =
        &g_compositor_snapshot;

    uint32_t region_count;

    if (!g_window_server.kernel_ready ||
        !g_window_server.dirty ||
        g_window_server.composing ||
        g_window_server.framebuffer == 0) {
        return false;
    }

    if (g_window_server.damage_full) {
        region_count = 1U;
    } else {
        region_count =
            g_window_server.damage_count;
    }

    if (region_count == 0U) {
        g_window_server.dirty = false;
        return false;
    }

    /*
     * A compositor is now active. Other CPUs may continue to mutate window
     * state and append new damage, but they cannot start another compositor.
     */
    g_window_server.composing = true;

    snapshot->window_count = 0U;

    snapshot->focused_identifier =
        g_window_server.focused_identifier;

    snapshot->pointer_x =
        g_window_server.pointer_x;

    snapshot->pointer_y =
        g_window_server.pointer_y;

    snapshot->desktop_hovered_app =
        g_window_server.desktop_hovered_app;

    snapshot->dragging_identifier =
        g_window_server.dragging_identifier;

    /*
     * Snapshot the current damage before clearing the producer side.
     */
    if (g_window_server.damage_full) {
        snapshot->damage_count = 1U;

        snapshot->damage_rects[0].left = 0U;
        snapshot->damage_rects[0].top = 0U;

        snapshot->damage_rects[0].right =
            g_window_server.display_width;

        snapshot->damage_rects[0].bottom =
            g_window_server.display_height;
    } else {
        snapshot->damage_count =
            g_window_server.damage_count;

        for (uint32_t index = 0U;
             index < snapshot->damage_count;
             ++index) {

            snapshot->damage_rects[index] =
                g_window_server.damage_rects[index];
        }
    }

    /*
     * Snapshot the visible scene.
     *
     * The registry already owns a reference while window_lock is held.
     * Take one extra reference for the unlocked compositor.  This keeps
     * window->section and its VM_OBJECT_SHARED alive even if the process
     * closes the window during this frame.
     */
    for (uint32_t index = 0U;
         index < g_window_server.count;
         ++index) {

        window_server_window_t *window =
            g_window_server.windows[index];

        compositor_window_view_t *view;

        if (window == 0 ||
            window->minimized ||
            (window->flags & OS_WINDOW_VISIBLE) == 0U) {
            continue;
        }

        if (snapshot->window_count >=
            WINDOW_SERVER_MAX_WINDOWS) {
            break;
        }

        object_get(window);

        view =
            &snapshot->windows[
                snapshot->window_count++];

        view->reference = window;
        view->section = window->section;

        view->identifier =
            window->identifier;

        view->x = window->x;
        view->y = window->y;

        view->width =
            window->width;

        view->height =
            window->height;

        view->flags =
            window->flags;

        view->background =
            window->background;

        view->buffer_size =
            window->buffer_size;

        view->owner_address =
            window->owner_address;

        view->resize_pending =
            window->resize_pending;

        view->maximized =
            window->maximized;

        for (uint32_t title_index = 0U;
             title_index <
                 sizeof(view->title);
             ++title_index) {

            view->title[title_index] =
                window->title[title_index];

            if (window->title[title_index] ==
                '\0') {
                break;
            }
        }

        view->title[
            sizeof(view->title) - 1U] =
            '\0';
    }

    /*
     * Consume only the damage represented by this snapshot.
     *
     * From this point forward, window_mark_*() writes into a fresh producer
     * damage set.  finish() intentionally does NOT clear that new state.
     */
    g_window_server.damage_count = 0U;
    g_window_server.damage_full = false;
    g_window_server.dirty = false;

    return true;
}




/*
 * Copy one XRGB8888 scanline from normal WB RAM into the GOP WC mapping.
 *
 * The framebuffer destination is write-combining memory.  Use integer
 * non-temporal stores so the copy does not pollute the normal CPU caches.
 *
 * No XMM/YMM state is touched: MOVNTI uses general-purpose registers.
 * Ordering is provided once for the complete frame by the sfence in
 * compositor_commit_snapshot().
 */
static inline void compositor_wc_store64(volatile uint64_t *destination,
                                         uint64_t value) {
    __asm__ volatile (
        "movnti %1, %0"
        : "=m"(*destination)
        : "r"(value)
        : "memory");
}

/*
 * WB composite framebuffer -> GPR.
 *
 * x86-64 supports unaligned 64-bit loads, so this remains correct even for
 * an unusual framebuffer/base alignment.  In the normal case both buffers
 * are naturally aligned and this becomes one MOVQ.
 */
static inline uint64_t compositor_wb_load64(
    const volatile uint32_t *source) {

    uint64_t value;

    __asm__ volatile (
        "movq %1, %0"
        : "=r"(value)
        : "m"(*(const volatile uint64_t *)(const void *)source)
        : "memory");

    return value;
}



static void compositor_copy_wc_scanline(
    volatile uint32_t *destination,
    const volatile uint32_t *source,
    uint32_t pixels) {

    if (destination == 0 ||
        source == 0 ||
        pixels == 0U) {
        return;
    }

    /*
     * First obtain natural qword alignment.
     *
     * destination/source advance together, and both framebuffers use the
     * same 32-bit pixel layout, so after this point pair loads/stores are
     * naturally aligned whenever the starting parity differed.
     */
    if (((uintptr_t)destination & 7U) != 0U &&
        pixels != 0U) {
        *destination++ = *source++;
        --pixels;
    }

    /*
     * Align the WC destination to a 64-byte cache-line boundary before the
     * main loop.  MOVNTI is still used here, so we do not introduce cached
     * stores into the WC bulk path.
     */
    while (pixels >= 2U &&
           ((uintptr_t)destination & 63U) != 0U) {

        uint64_t value =
            compositor_wb_load64(source);

        compositor_wc_store64(
            (volatile uint64_t *)(void *)destination,
            value);

        destination += 2;
        source += 2;
        pixels -= 2U;
    }

    /*
     * Main 64-byte path:
     *
     *     16 XRGB8888 pixels
     *       = 64 bytes
     *       = 8 MOVNTI qword stores
     *
     * One loop iteration now consumes one complete cache line.
     */
    while (pixels >= 16U) {
        uint64_t v0 =
            compositor_wb_load64(source + 0U);

        uint64_t v1 =
            compositor_wb_load64(source + 2U);

        uint64_t v2 =
            compositor_wb_load64(source + 4U);

        uint64_t v3 =
            compositor_wb_load64(source + 6U);

        uint64_t v4 =
            compositor_wb_load64(source + 8U);

        uint64_t v5 =
            compositor_wb_load64(source + 10U);

        uint64_t v6 =
            compositor_wb_load64(source + 12U);

        uint64_t v7 =
            compositor_wb_load64(source + 14U);

        volatile uint64_t *out =
            (volatile uint64_t *)(void *)destination;

        compositor_wc_store64(out + 0, v0);
        compositor_wc_store64(out + 1, v1);
        compositor_wc_store64(out + 2, v2);
        compositor_wc_store64(out + 3, v3);
        compositor_wc_store64(out + 4, v4);
        compositor_wc_store64(out + 5, v5);
        compositor_wc_store64(out + 6, v6);
        compositor_wc_store64(out + 7, v7);

        destination += 16;
        source += 16;
        pixels -= 16U;
    }

    /*
     * 32-byte remainder.
     */
    if (pixels >= 8U) {
        uint64_t v0 =
            compositor_wb_load64(source + 0U);

        uint64_t v1 =
            compositor_wb_load64(source + 2U);

        uint64_t v2 =
            compositor_wb_load64(source + 4U);

        uint64_t v3 =
            compositor_wb_load64(source + 6U);

        volatile uint64_t *out =
            (volatile uint64_t *)(void *)destination;

        compositor_wc_store64(out + 0, v0);
        compositor_wc_store64(out + 1, v1);
        compositor_wc_store64(out + 2, v2);
        compositor_wc_store64(out + 3, v3);

        destination += 8;
        source += 8;
        pixels -= 8U;
    }

    /*
     * Remaining qwords.
     */
    while (pixels >= 2U) {
        uint64_t value =
            compositor_wb_load64(source);

        compositor_wc_store64(
            (volatile uint64_t *)(void *)destination,
            value);

        destination += 2;
        source += 2;
        pixels -= 2U;
    }

    /*
     * At most one final XRGB8888 pixel remains.
     *
     * sfence deliberately remains at frame/commit level rather than here.
     */
    if (pixels != 0U) {
        *destination = *source;
    }
}


static void compositor_commit_snapshot(void) {
    bool commit_as_move_transaction;

    if (g_window_server.framebuffer == 0 ||
        g_window_server.composite_framebuffer == 0 ||
        g_window_server.composite_framebuffer ==
            g_window_server.framebuffer ||
        g_compositor_snapshot.damage_count == 0U) {
        return;
    }

    commit_as_move_transaction =
        g_compositor_snapshot.dragging_identifier != 0U &&
        g_compositor_snapshot.damage_count > 1U;

    /*
     * Only GOP publication is non-preemptible.  All expensive scene
     * composition has already completed in normal WB memory.
     */
    sched_preempt_disable();

    if (commit_as_move_transaction) {
        uint32_t left =
            g_window_server.display_width;

        uint32_t top =
            g_window_server.display_height;

        uint32_t right = 0U;
        uint32_t bottom = 0U;

        /*
         * A window move usually contributes:
         *
         *   old window
         *   new window
         *   old cursor
         *   new cursor
         *
         * They were rendered independently into the backbuffer for
         * efficiency, but publishing them independently lets scanout observe
         * the old area after it has been erased and before the new titlebar
         * has arrived.  Publish their bounding transaction instead.
         */
        for (uint32_t region = 0U;
             region < g_compositor_snapshot.damage_count;
             ++region) {

            const window_damage_rect_t *rect =
                &g_compositor_snapshot.damage_rects[region];

            if (rect->left < left) {
                left = rect->left;
            }

            if (rect->top < top) {
                top = rect->top;
            }

            if (rect->right > right) {
                right = rect->right;
            }

            if (rect->bottom > bottom) {
                bottom = rect->bottom;
            }
        }

        if (left < right && top < bottom) {
            for (uint32_t row = top;
                 row < bottom;
                 ++row) {

                volatile uint32_t *destination =
                    g_window_server.framebuffer +
                    (uint64_t)row *
                        g_window_server.display_stride +
                    left;

                const volatile uint32_t *source =
                    g_window_server.composite_framebuffer +
                    (uint64_t)row *
                        g_window_server.display_stride +
                    left;

                compositor_copy_wc_scanline(
                    destination,
                    source,
                    right - left);
            }
        }
    } else {
        /*
         * Ordinary unrelated damages remain separate.  This preserves the
         * small-damage optimization for cursor movement, app updates, etc.
         */
        for (uint32_t region = 0U;
             region < g_compositor_snapshot.damage_count;
             ++region) {

            const window_damage_rect_t *rect =
                &g_compositor_snapshot.damage_rects[region];

            uint32_t left = rect->left;
            uint32_t top = rect->top;
            uint32_t right = rect->right;
            uint32_t bottom = rect->bottom;

            if (left >= right || top >= bottom) {
                continue;
            }

            for (uint32_t row = top;
                 row < bottom;
                 ++row) {

                volatile uint32_t *destination =
                    g_window_server.framebuffer +
                    (uint64_t)row *
                        g_window_server.display_stride +
                    left;

                const volatile uint32_t *source =
                    g_window_server.composite_framebuffer +
                    (uint64_t)row *
                        g_window_server.display_stride +
                    left;

                compositor_copy_wc_scanline(
                    destination,
                    source,
                    right - left);
            }
        }
    }

    /*
     * Complete all WC stores before scanout may observe the transaction.
     */
    __asm__ volatile (
        "sfence"
        :
        :
        : "memory");

    sched_preempt_enable();
}


static void compositor_render_snapshot(void) {
    bool direct_framebuffer =
        g_window_server.composite_framebuffer ==
        g_window_server.framebuffer;

    /*
     * Normal path:
     *     compose framebuffer (RAM) -> preemptible
     *     GOP publication          -> one short non-preemptible transaction
     *
     * Low-memory fallback has no private compose buffer, so preserve atomic
     * rendering there rather than expose intermediate drawing operations.
     */
    if (direct_framebuffer) {
        sched_preempt_disable();
    }

    for (uint32_t index = 0U;
         index < g_compositor_snapshot.damage_count;
         ++index) {

        const window_damage_rect_t *rect =
            &g_compositor_snapshot.damage_rects[index];

        g_compositor_snapshot.damage_left =
            rect->left;

        g_compositor_snapshot.damage_top =
            rect->top;

        g_compositor_snapshot.damage_right =
            rect->right;

        g_compositor_snapshot.damage_bottom =
            rect->bottom;

        compositor_region_locked();
    }

    if (direct_framebuffer) {
        __asm__ volatile (
            "sfence"
            :
            :
            : "memory");

        sched_preempt_enable();
    } else {
        compositor_commit_snapshot();
    }
}

static void compositor_snapshot_finish(void) {
    /*
     * Release the unlocked compositor's lifetime references.
     *
     * No window_server fields are read through these objects after this loop.
     */
    for (uint32_t index = 0U;
         index < g_compositor_snapshot.window_count;
         ++index) {

        compositor_window_view_t *view =
            &g_compositor_snapshot.windows[index];

        if (view->reference != 0) {
            object_put(view->reference);
            view->reference = 0;
        }

        view->section = 0;
    }

    g_compositor_snapshot.window_count = 0U;
    g_compositor_snapshot.damage_count = 0U;

    /*
     * Do not clear dirty/damage here.
     *
     * Input, resize, WINDOW_UPDATE or process exit may have generated another
     * frame while the compositor was running.
     */
    window_lock();

    g_window_server.composing = false;

    window_unlock();
}


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


static void window_enqueue_event_locked(window_server_window_t *window,
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
}


/*
 * Queue one cooperative window-close request.
 *
 * No handle is closed and no process is terminated here. Ring3 remains the
 * owner of normal shutdown policy.
 */
static void window_enqueue_close_request_locked(
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
    window->compositor_cache = 0;
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

kstatus_t window_server_lookup(uint32_t identifier, window_server_window_t **out) {
    window_server_window_t *window;
    if (out == 0 || !window_server_init()) return K_EINVAL;
    window_lock();
    window = find_window_locked(identifier);
    if (window == 0 || !object_try_get(window)) {
        window_unlock();
        return K_ENOENT;
    }
    window_unlock();
    *out = window;
    return K_OK;
}

void window_server_put(window_server_window_t *window) {
    object_put(window);
}

void window_server_handle_closed(window_server_window_t *window) {
    if (window == 0 || !window_server_init()) return;
    window_lock();
    remove_window_locked(window);
    window_unlock();
}

void window_server_close_process(process_t *owner) {
    if (owner == 0 || !window_server_init()) return;
    window_lock();
    for (uint32_t index = 0U; index < g_window_server.count;) {
        window_server_window_t *window = g_window_server.windows[index];
        if (window == 0 || window->owner != owner) {
            ++index;
            continue;
        }
        /* remove_window_locked() compacts the array, so keep the index. */
        remove_window_locked(window);
    }
    window_unlock();
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
    old_focused = find_window_locked(g_window_server.focused_identifier);
    window_mark_moved_rect_locked(old_x, old_y, x, y,
                                  window_outer_width(window->width, window->flags),
                                  window_outer_height(window->height, window->flags));
    window->x = x;
    window->y = y;
    if (visible != 0U) window->flags |= OS_WINDOW_VISIBLE;
    else window->flags &= ~OS_WINDOW_VISIBLE;
    if (visible == 0U && g_window_server.focused_identifier == window->identifier) {
        g_window_server.focused_identifier = 0U;
        for (uint32_t i = g_window_server.count; i != 0U; --i) {
            window_server_window_t *candidate = g_window_server.windows[i - 1U];
            if ((candidate->flags & OS_WINDOW_VISIBLE) != 0U) {
                g_window_server.focused_identifier = candidate->identifier;
                break;
            }
        }
    }
    window_mark_window_locked(old_focused);
    window_mark_window_locked(window);
    window_unlock();
    return K_OK;
}

kstatus_t window_server_focus(uint32_t identifier) {
    window_server_window_t *window;
    if (!window_server_init()) return K_EINVAL;
    window_lock();
    window = find_window_locked(identifier);
    if (window == 0 || (window->flags & OS_WINDOW_VISIBLE) == 0U) {
        window_unlock();
        return K_ENOENT;
    }
    focus_locked(window);
    window_unlock();
    return K_OK;
}

kstatus_t window_server_map(window_server_window_t *window, process_t *process,
                            uint64_t requested_address, uint64_t *mapped_address) {
    vm_object_t *object;
    vaddr_t address;
    kstatus_t status;
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
    *mapped_address = (uint64_t)address;
    return K_OK;
}

kstatus_t window_server_dispatch(uint32_t identifier, const os_input_event_t *event) {
    window_server_window_t *window;
    if (event == 0 || !window_server_init()) return K_EINVAL;
    window_lock();
    window = find_window_locked(identifier);
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
    window_unlock();
    (void)wake_all(&g_window_server.event_waitq);
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
    if (process == 0 || event == 0 || !window_server_init()) return K_EINVAL;
    context.process = process;
    context.identifier = identifier;
    context.event = event;
    if (window_event_available(&context)) return K_OK;
    if (timeout_ns == 0U) return K_EAGAIN;
    return wait_on_queue(&g_window_server.event_waitq, window_event_available,
                         &context, timeout_ns);
}

kstatus_t window_server_update(process_t *process, uint32_t identifier,
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

        g_window_server.dragging_identifier = 0U;
        g_window_server.drag_offset_x = 0;
        g_window_server.drag_offset_y = 0;
        g_window_server.resize_edges = 0U;
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

        g_window_server.focused_identifier = 0U;

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
            g_window_server.focused_identifier =
                candidate->identifier;

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

        g_window_server.dragging_identifier =
            0U;

        g_window_server.drag_offset_x =
            0;

        g_window_server.drag_offset_y =
            0;

        g_window_server.resize_edges =
            0U;
    }

    window_mark_window_locked(
        window);

    window_enqueue_resize_event_locked(
        window);

    return true;
}


static uint32_t window_resize_edges_locked(const window_server_window_t *window,
                                           uint32_t x, uint32_t y) {
    int64_t relative_x;
    int64_t relative_y;
    int64_t outer_width;
    int64_t outer_height;
    uint32_t edges = 0U;

    if (window == 0 ||
        window->maximized ||
        (window->flags & OS_WINDOW_RESIZABLE) == 0U) {

        return 0U;
    }

    relative_x = (int64_t)x - window->x;
    relative_y = (int64_t)y - window->y;
    outer_width = (int64_t)window_outer_width(window->width, window->flags);
    outer_height =
        (int64_t)window_outer_height(window->height, window->flags);
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
    right = left +
            (int64_t)window_outer_width(window->width, window->flags);
    bottom =
        top +
        (int64_t)window_outer_height(window->height, window->flags);

    if ((g_window_server.resize_edges & WINDOW_RESIZE_LEFT) != 0U) {
        left += delta_x;
        minimum = right - (int64_t)max_width -
                  (int64_t)window_frame_extra(window->flags);
        maximum = right - (int64_t)min_width -
                  (int64_t)window_frame_extra(window->flags);
        if (left < minimum) left = minimum;
        if (left > maximum) left = maximum;
    } else if ((g_window_server.resize_edges & WINDOW_RESIZE_RIGHT) != 0U) {
        right += delta_x;
        minimum = left + (int64_t)min_width +
                  (int64_t)window_frame_extra(window->flags);
        maximum = left + (int64_t)max_width +
                  (int64_t)window_frame_extra(window->flags);
        if (right < minimum) right = minimum;
        if (right > maximum) right = maximum;
    }

    if ((g_window_server.resize_edges & WINDOW_RESIZE_TOP) != 0U) {
        top += delta_y;
        minimum =
            bottom -
            (int64_t)max_height -
            (int64_t)window_frame_extra(window->flags) -
            (int64_t)window_titlebar_height(window->flags);
        maximum =
            bottom -
            (int64_t)min_height -
            (int64_t)window_frame_extra(window->flags) -
            (int64_t)window_titlebar_height(window->flags);
        if (top < minimum) top = minimum;
        if (top > maximum) top = maximum;
    } else if ((g_window_server.resize_edges & WINDOW_RESIZE_BOTTOM) != 0U) {
        bottom += delta_y;
        minimum =
            top +
            (int64_t)min_height +
            (int64_t)window_frame_extra(window->flags) +
            (int64_t)window_titlebar_height(window->flags);
        maximum =
            top +
            (int64_t)max_height +
            (int64_t)window_frame_extra(window->flags) +
            (int64_t)window_titlebar_height(window->flags);
        if (bottom < minimum) bottom = minimum;
        if (bottom > maximum) bottom = maximum;
    }

    new_width = (uint32_t)(right - left -
                           (int64_t)window_frame_extra(window->flags));
    new_height =
        (uint32_t)(
            bottom -
            top -
            (int64_t)window_frame_extra(window->flags) -
            (int64_t)window_titlebar_height(window->flags));
    if (left == old_x && top == old_y &&
        new_width == old_width && new_height == old_height) {
        return false;
    }

    window_mark_rect_locked(old_x, old_y,
                            window_outer_width(old_width, window->flags),
                            window_outer_height(old_height, window->flags));
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

static void route_input_locked(const input_event_t *event) {
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
                g_window_server.desktop_hovered_app = hovered;
            }
        }

        /*
         * A title control is a click candidate for a short time, then becomes
         * a drag capture.  This applies to both legacy compositor controls
         * and client-drawn header buttons.  The decision is timestamp-based,
         * so a small pointer movement does not accidentally turn a click into
         * a drag.
         */
        if (g_window_server.dragging_identifier == 0U &&
            g_window_server.title_pressed_identifier != 0U) {
            window_server_window_t *pressed =
                find_window_locked(
                    g_window_server.title_pressed_identifier);
            if (window_title_capture_elapsed_locked(event)) {
                if (pressed != 0 &&
                    (pressed->flags & OS_WINDOW_VISIBLE) != 0U &&
                    !pressed->maximized) {
                    g_window_server.dragging_identifier =
                        pressed->identifier;
                    g_window_server.drag_offset_x =
                        (int32_t)g_window_server.title_pressed_pointer_x -
                        pressed->x;
                    g_window_server.drag_offset_y =
                        (int32_t)g_window_server.title_pressed_pointer_y -
                        pressed->y;
                    g_window_server.resize_edges = 0U;
                    window_clear_title_capture_locked();
                } else {
                    window_clear_title_capture_locked();
                }
            }
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
                    window_outer_width(dragged->width, dragged->flags),
                    window_outer_height(dragged->height, dragged->flags));
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
                    !window_title_capture_elapsed_locked(event)) {

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
                g_window_server.dragging_identifier = target->identifier;
                g_window_server.drag_offset_x = 0;
                g_window_server.drag_offset_y = 0;
                g_window_server.resize_edges = resize_edges;
                deliver_input = false;
            } else if (
                window_client_decorations(target->flags) &&
                (int64_t)g_window_server.pointer_y - target->y >= 0 &&
                (int64_t)g_window_server.pointer_y - target->y <
                    WINDOW_CLIENT_DRAG_REGION_HEIGHT &&
                !window_client_drag_region(
                    target,
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
                     target,
                     g_window_server.pointer_x,
                     g_window_server.pointer_y) ||
                 (!window_client_decorations(target->flags) &&
                  (int64_t)g_window_server.pointer_y - target->y <
                  WINDOW_DRAG_REGION_HEIGHT))) {
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
            if (g_window_server.title_pressed_client) {
                window_server_window_t *pressed =
                    find_window_locked(
                        g_window_server.title_pressed_identifier);
                input_event_t press_event =
                    g_window_server.title_pressed_event;

                if (pressed != 0 && target != 0 &&
                    pressed->identifier == target->identifier &&
                    !window_title_capture_elapsed_locked(event)) {
                    press_event.value = INPUT_VALUE_PRESS;
                    window_enqueue_event_locked(pressed, &press_event);
                    window_enqueue_event_locked(pressed, event);
                }
                window_clear_title_capture_locked();
                deliver_input = false;
            } else if (g_window_server.dragging_identifier != 0U) {
                target = find_window_locked(g_window_server.dragging_identifier);
                deliver_input = false;
            }
            window_clear_title_capture_locked();
            g_window_server.dragging_identifier = 0U;
            g_window_server.drag_offset_x = 0;
            g_window_server.drag_offset_y = 0;
            g_window_server.resize_edges = 0U;
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
    if (deliver_input && target != 0) {
        window_enqueue_event_locked(target, event);
    }
}


typedef struct window_motion_batch {
    bool active;
    uint32_t device_id;
    uint16_t flags;
    uint64_t timestamp;
    int64_t delta_x;
    int64_t delta_y;
} window_motion_batch_t;

/*
 * Feed an accumulated relative axis back through the normal window routing
 * path.  int64_t accumulation prevents a burst of raw events from overflowing
 * before it is emitted as one or more ABI-sized int32_t events.
 */
static void window_route_motion_axis_locked(
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


/*
 * Apply one accumulated X/Y motion burst to an active Ring0 decoration
 * capture.
 *
 * Ordinary pointer motion deliberately keeps the existing per-axis routing
 * semantics.  Only drag/resize takes this fast path, because those relative
 * events are already consumed by Ring0 and are never delivered to the client.
 *
 * This removes the artificial intermediate:
 *
 *     (old_x, old_y)
 *        -> (new_x, old_y)
 *        -> (new_x, new_y)
 *
 * and replaces it with one geometry transition:
 *
 *     (old_x, old_y)
 *        -> (new_x, new_y)
 */
static bool window_route_drag_motion_batch_locked(
    const window_motion_batch_t *batch) {

    window_server_window_t *dragged;

    uint32_t old_pointer_x;
    uint32_t old_pointer_y;

    int64_t next_x;
    int64_t next_y;

    int32_t actual_delta_x;
    int32_t actual_delta_y;

    if (batch == 0 ||
        g_window_server.dragging_identifier == 0U) {
        return false;
    }

    dragged =
        find_window_locked(
            g_window_server.dragging_identifier);

    /*
     * A stale capture is not handled here. Clear it and let the caller fall
     * back to the normal per-axis path.
     */
    if (dragged == 0 ||
        (dragged->flags & OS_WINDOW_VISIBLE) == 0U) {

        g_window_server.dragging_identifier = 0U;
        g_window_server.drag_offset_x = 0;
        g_window_server.drag_offset_y = 0;
        g_window_server.resize_edges = 0U;

        return false;
    }

    old_pointer_x =
        g_window_server.pointer_x;

    old_pointer_y =
        g_window_server.pointer_y;

    /*
     * Apply X and Y to the pointer before changing window geometry.
     */
    next_x =
        (int64_t)old_pointer_x +
        batch->delta_x;

    next_y =
        (int64_t)old_pointer_y +
        batch->delta_y;

    if (next_x < 0) {
        next_x = 0;
    }

    if (next_y < 0) {
        next_y = 0;
    }

    if (g_window_server.display_width == 0U) {
        next_x = 0;
    } else if (next_x >=
               (int64_t)g_window_server.display_width) {

        next_x =
            (int64_t)
                g_window_server.display_width -
            1;
    }

    if (g_window_server.display_height == 0U) {
        next_y = 0;
    } else if (next_y >=
               (int64_t)g_window_server.display_height) {

        next_y =
            (int64_t)
                g_window_server.display_height -
            1;
    }

    g_window_server.pointer_x =
        (uint32_t)next_x;

    g_window_server.pointer_y =
        (uint32_t)next_y;

    /*
     * Decoration capture can never hover a desktop icon.  The old per-axis
     * route also cleared this on its first motion event.
     */
    if (g_window_server.desktop_hovered_app !=
        DESKTOP_APP_NONE) {

        desktop_mark_app_locked(
            g_window_server.desktop_hovered_app);

        g_window_server.desktop_hovered_app =
            DESKTOP_APP_NONE;
    }

    actual_delta_x =
        (int32_t)(
            (int64_t)g_window_server.pointer_x -
            (int64_t)old_pointer_x);

    actual_delta_y =
        (int32_t)(
            (int64_t)g_window_server.pointer_y -
            (int64_t)old_pointer_y);

    if (g_window_server.resize_edges != 0U) {
        /*
         * window_resize_locked() already accepts both axes together, so call
         * it exactly once for this batch.
         */
        if (actual_delta_x != 0 ||
            actual_delta_y != 0) {

            (void)window_resize_locked(
                dragged,
                actual_delta_x,
                actual_delta_y);
        }
    } else {
        int32_t old_x =
            dragged->x;

        int32_t old_y =
            dragged->y;

        int32_t new_x =
            (int32_t)g_window_server.pointer_x -
            g_window_server.drag_offset_x;

        int32_t new_y =
            (int32_t)g_window_server.pointer_y -
            g_window_server.drag_offset_y;

        /*
         * One old/new damage pair for the complete diagonal move.
         */
        if (old_x != new_x ||
            old_y != new_y) {

            dragged->x =
                new_x;

            dragged->y =
                new_y;

            window_mark_moved_rect_locked(
                old_x,
                old_y,
                new_x,
                new_y,
                window_outer_width(dragged->width, dragged->flags),
                window_outer_height(dragged->height, dragged->flags));

            dragged->dirty =
                true;
        }
    }

    /*
     * Cursor also moves from old -> final position only once.
     */
    if (old_pointer_x !=
            g_window_server.pointer_x ||
        old_pointer_y !=
            g_window_server.pointer_y) {

        window_mark_moved_cursor_locked(
            old_pointer_x,
            old_pointer_y,
            g_window_server.pointer_x,
            g_window_server.pointer_y);
    }

    /*
     * Previously route_input_locked() called this once for X and once for Y.
     */
    window_coalesce_damage_locked();

    return true;
}


static void window_flush_motion_batch_locked(
    window_motion_batch_t *batch) {

    if (batch == 0 ||
        !batch->active) {
        return;
    }

    /*
     * Decoration movement is consumed by Ring0, so X/Y may safely be applied
     * as one geometry update.
     */
    if (window_route_drag_motion_batch_locked(
            batch)) {

        batch->active = false;
        batch->delta_x = 0;
        batch->delta_y = 0;

        return;
    }

    /*
     * Keep ordinary client-facing relative input exactly as before.
     */
    window_route_motion_axis_locked(
        batch,
        INPUT_REL_X,
        batch->delta_x);

    window_route_motion_axis_locked(
        batch,
        INPUT_REL_Y,
        batch->delta_y);

    batch->active = false;
    batch->delta_x = 0;
    batch->delta_y = 0;
}

void window_server_pump_input(void) {
    input_event_t event;
    window_motion_batch_t motion = {0};
    bool wake = false;
    bool compose = false;
    uint32_t consumed = 0U;

    if (!window_server_kernel_ready()) return;

    /*
     * Raw HID/PS2 backends commonly enqueue X and Y as separate events.
     * Accumulate consecutive pointer motion and route the whole burst only
     * once per axis.
     *
     * Any non-X/Y event is an ordering barrier.  Therefore button presses,
     * wheel events and keys always observe all preceding pointer movement.
     */
    while (consumed < WINDOW_EVENT_CAPACITY &&
           input_core_pop(&event) == K_OK) {
        bool is_motion =
            event.type == INPUT_EVENT_RELATIVE &&
            (event.code == INPUT_REL_X ||
             event.code == INPUT_REL_Y);

        ++consumed;
        wake = true;

        if (is_motion) {
            /*
             * Never merge events belonging to different physical devices or
             * carrying different backend flags.
             */
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

            /*
             * Use the newest timestamp for the merged motion event.
             */
            motion.timestamp = event.timestamp;

            if (event.code == INPUT_REL_X) {
                motion.delta_x += event.value;
            } else {
                motion.delta_y += event.value;
            }

            continue;
        }

        /*
         * Button/key/wheel/absolute events are ordering barriers.
         */
        window_lock();

        window_flush_motion_batch_locked(&motion);
        route_input_locked(&event);

        window_unlock();
    }

    /*
     * Flush a trailing mouse-motion burst once, under one lock acquisition.
     */
    if (motion.active) {
        window_lock();
        window_flush_motion_batch_locked(&motion);
        window_unlock();
    }

    if (wake) {
        (void)wake_all(&g_window_server.event_waitq);
    }

    for (;;) {
        uint32_t app;
        bool cycle_focus;

        window_lock();

        app = g_window_server.desktop_pending_launch;
        g_window_server.desktop_pending_launch = DESKTOP_APP_NONE;

        cycle_focus =
            g_window_server.desktop_focus_cycle_requested;
        g_window_server.desktop_focus_cycle_requested = false;

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

    /*
     * Only the scene snapshot is taken while holding window_lock.
     *
     * The expensive desktop/window/framebuffer work below is preemptible.
     */
    window_lock();

    compose =
        compositor_snapshot_begin_locked();

    window_unlock();

    if (compose) {
        compositor_render_snapshot();
        compositor_snapshot_finish();
    }
}
