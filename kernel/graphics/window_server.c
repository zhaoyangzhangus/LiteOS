#include <kernel/window_server.h>
#include <arch/x86_64/paging.h>
#include <kernel/display.h>
#include <kernel/input.h>
#include <kernel/kmem.h>
#include <kernel/sched.h>
#include <kernel/vm.h>

#define WINDOW_DAMAGE_MAX_RECTS 16U
#define WINDOW_FRAME_BORDER 1U
#define WINDOW_FRAME_EXTRA (WINDOW_FRAME_BORDER * 2U)
#define WINDOW_CURSOR_WIDTH 24U
#define WINDOW_CURSOR_HEIGHT 24U
#define WINDOW_CURSOR_HOTSPOT_X 3U
#define WINDOW_CURSOR_HOTSPOT_Y 1U

typedef struct window_damage_rect {
    uint32_t left;
    uint32_t top;
    uint32_t right;
    uint32_t bottom;
} window_damage_rect_t;




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
    uint32_t dragging_identifier;
    int32_t drag_offset_x;
    int32_t drag_offset_y;
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
        g_window_server.dragging_identifier = 0U;
        g_window_server.drag_offset_x = 0;
        g_window_server.drag_offset_y = 0;
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
                if ((candidate->flags & OS_WINDOW_VISIBLE) != 0U) {
                    g_window_server.focused_identifier = candidate->identifier;
                    new_focused = candidate;
                    break;
                }
            }
        }
        if (g_window_server.dragging_identifier == window->identifier) {
            g_window_server.dragging_identifier = 0U;
            g_window_server.drag_offset_x = 0;
            g_window_server.drag_offset_y = 0;
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
        if (candidate.right < current->left || current->right < candidate.left ||
            candidate.bottom < current->top || current->bottom < candidate.top) {
            continue;
        }
        if (candidate.left < current->left) current->left = candidate.left;
        if (candidate.top < current->top) current->top = candidate.top;
        if (candidate.right > current->right) current->right = candidate.right;
        if (candidate.bottom > current->bottom) current->bottom = candidate.bottom;
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
                            window->width + WINDOW_FRAME_EXTRA,
                            window->height + WINDOW_FRAME_EXTRA);
}

static void window_mark_surface_locked(const window_server_window_t *window,
                                       int32_t x, int32_t y,
                                       uint32_t width, uint32_t height) {
    int64_t left;
    int64_t top;
    int64_t right;
    int64_t bottom;
    if (window == 0 || width == 0U || height == 0U) return;
    left = (int64_t)window->x + WINDOW_FRAME_BORDER + x;
    top = (int64_t)window->y + WINDOW_FRAME_BORDER + y;
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

/* 移动对象的旧位置和新位置使用一个连续 damage，避免两个 framebuffer
 * 提交之间暴露出半更新画面。普通窗口更新仍可使用多个独立矩形。 */
static void window_mark_moved_rect_locked(int32_t old_x, int32_t old_y,
                                          int32_t new_x, int32_t new_y,
                                          uint32_t width, uint32_t height) {
    int64_t left = old_x < new_x ? old_x : new_x;
    int64_t top = old_y < new_y ? old_y : new_y;
    int64_t old_right = (int64_t)old_x + width;
    int64_t new_right = (int64_t)new_x + width;
    int64_t old_bottom = (int64_t)old_y + height;
    int64_t new_bottom = (int64_t)new_y + height;
    int64_t right = old_right > new_right ? old_right : new_right;
    int64_t bottom = old_bottom > new_bottom ? old_bottom : new_bottom;
    if (right <= left || bottom <= top) return;
    window_mark_rect_locked((int32_t)left, (int32_t)top,
                            (uint32_t)(right - left),
                            (uint32_t)(bottom - top));
}

static void window_mark_moved_cursor_locked(uint32_t old_x, uint32_t old_y,
                                            uint32_t new_x, uint32_t new_y) {
    /* pointer_x/y 是 Linux 光标的 hotspot，不是 sprite 左上角。 */
    window_mark_moved_rect_locked(
        (int32_t)old_x - (int32_t)WINDOW_CURSOR_HOTSPOT_X,
        (int32_t)old_y - (int32_t)WINDOW_CURSOR_HOTSPOT_Y,
        (int32_t)new_x - (int32_t)WINDOW_CURSOR_HOTSPOT_X,
        (int32_t)new_y - (int32_t)WINDOW_CURSOR_HOTSPOT_Y,
                                  WINDOW_CURSOR_WIDTH, WINDOW_CURSOR_HEIGHT);
}

/* 一次鼠标事务只允许一次 framebuffer 区域提交。焦点、任务栏、被拖动
 * 窗口和光标可能分别产生 damage；分开提交会让 GOP 扫描到半帧状态。 */
static void window_coalesce_damage_locked(void) {
    uint32_t left;
    uint32_t top;
    uint32_t right;
    uint32_t bottom;
    if (g_window_server.damage_full || g_window_server.damage_count <= 1U) return;
    left = g_window_server.display_width;
    top = g_window_server.display_height;
    right = 0U;
    bottom = 0U;
    for (uint32_t index = 0U; index < g_window_server.damage_count; ++index) {
        const window_damage_rect_t *rect = &g_window_server.damage_rects[index];
        if (rect->left < left) left = rect->left;
        if (rect->top < top) top = rect->top;
        if (rect->right > right) right = rect->right;
        if (rect->bottom > bottom) bottom = rect->bottom;
    }
    if (left >= right || top >= bottom) {
        g_window_server.damage_count = 0U;
        return;
    }
    g_window_server.damage_rects[0].left = left;
    g_window_server.damage_rects[0].top = top;
    g_window_server.damage_rects[0].right = right;
    g_window_server.damage_rects[0].bottom = bottom;
    g_window_server.damage_count = 1U;
}

static void compositor_blend_pixel_locked(int32_t x, int32_t y,
                                           uint32_t color, uint32_t alpha) {
    volatile uint32_t *destination;
    uint32_t current;
    uint32_t inverse;
    uint32_t red;
    uint32_t green;
    uint32_t blue;
    if (!g_window_server.kernel_ready ||
        g_window_server.composite_framebuffer == 0 || alpha == 0U ||
        x < 0 || y < 0 || x >= (int32_t)g_window_server.display_width ||
        y >= (int32_t)g_window_server.display_height ||
        (uint32_t)x < g_window_server.damage_left ||
        (uint32_t)x >= g_window_server.damage_right ||
        (uint32_t)y < g_window_server.damage_top ||
        (uint32_t)y >= g_window_server.damage_bottom) return;
    destination = g_window_server.composite_framebuffer +
        (uint64_t)(uint32_t)y * g_window_server.display_stride +
        (uint32_t)x;
    if (alpha >= 255U) {
        *destination = color;
        return;
    }
    current = *destination;
    inverse = 255U - alpha;
    red = ((((current >> 16) & 0xFFU) * inverse) +
           (((color >> 16) & 0xFFU) * alpha) + 127U) / 255U;
    green = ((((current >> 8) & 0xFFU) * inverse) +
             (((color >> 8) & 0xFFU) * alpha) + 127U) / 255U;
    blue = (((current & 0xFFU) * inverse) +
            ((color & 0xFFU) * alpha) + 127U) / 255U;
    *destination = (red << 16) | (green << 8) | blue;
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
    if (left < (int32_t)g_window_server.damage_left) {
        left = (int32_t)g_window_server.damage_left;
    }
    if (top < (int32_t)g_window_server.damage_top) {
        top = (int32_t)g_window_server.damage_top;
    }
    if (right > (int64_t)g_window_server.damage_right) {
        right = g_window_server.damage_right;
    }
    if (bottom > (int64_t)g_window_server.damage_bottom) {
        bottom = g_window_server.damage_bottom;
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
        volatile uint32_t *destination = g_window_server.composite_framebuffer +
            (uint64_t)(uint32_t)row * g_window_server.display_stride +
            (uint32_t)span_left;
        for (int64_t column = span_left; column < span_right; ++column) {
            *destination++ = color;
        }
    }
}

static window_server_window_t *window_at_locked(uint32_t x, uint32_t y) {
    for (uint32_t index = g_window_server.count; index != 0U; --index) {
        window_server_window_t *window = g_window_server.windows[index - 1U];
        int64_t relative_x;
        int64_t relative_y;
        uint32_t inset;
        if (window == 0 || (window->flags & OS_WINDOW_VISIBLE) == 0U) continue;
        relative_x = (int64_t)x - window->x;
        relative_y = (int64_t)y - window->y;
        if (relative_x < 0 || relative_y < 0 ||
            relative_x >= (int64_t)window->width + WINDOW_FRAME_EXTRA ||
            relative_y >= (int64_t)window->height + WINDOW_FRAME_EXTRA) {
            continue;
        }
        inset = compositor_corner_inset(
            (uint32_t)relative_y, window->width + WINDOW_FRAME_EXTRA,
            window->height + WINDOW_FRAME_EXTRA);
        if (relative_x >= (int64_t)inset &&
            relative_x < (int64_t)window->width + WINDOW_FRAME_EXTRA - inset) {
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
    if (window != 0 && (window->flags & OS_WINDOW_VISIBLE) != 0U) return window;
    for (uint32_t index = g_window_server.count; index != 0U; --index) {
        window = g_window_server.windows[index - 1U];
        if (window != 0 && (window->flags & OS_WINDOW_VISIBLE) != 0U) {
            g_window_server.focused_identifier = window->identifier;
            return window;
        }
    }
    return 0;
}

static void focus_locked(window_server_window_t *window) {
    window_server_window_t *old_focused;
    uint32_t position = 0U;
    if (window == 0 || (window->flags & OS_WINDOW_VISIBLE) == 0U) return;
    old_focused = find_window_locked(g_window_server.focused_identifier);
    for (; position < g_window_server.count; ++position) {
        if (g_window_server.windows[position] == window) break;
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

static bool window_source_pixel_locked(const window_server_window_t *window,
                                       uint64_t virtual_address,
                                       uint32_t *pixel,
                                       uint64_t *cached_page,
                                       uint8_t **cached_base) {
    paddr_t physical;
    uint64_t page = virtual_address & ~(uint64_t)(PAGE_SIZE - 1U);
    if (window == 0 || pixel == 0 || cached_page == 0 || cached_base == 0 ||
        window->owner == 0 || window->owner->vm == 0 ||
        window->owner_address == 0U) return false;
    if (*cached_page != page) {
        /* x86_translate_page 返回的物理地址包含传入地址的页内偏移。
         * 这里缓存的是整页基址，因此必须翻译页首地址，避免后面
         * 再加一次 offset 后跨页读取错误内容。 */
        if (x86_translate_page(window->owner->vm->root_table,
                               (vaddr_t)page, &physical, 0) != K_OK) {
            vm_fault_info_t fault = {
                .address = (vaddr_t)page,
                .access = VM_PROT_READ,
                .cpu_error = 0U,
            };
            if (vm_handle_fault(window->owner->vm, &fault) != K_OK ||
                x86_translate_page(window->owner->vm->root_table,
                                   (vaddr_t)page, &physical, 0) != K_OK) {
                return false;
            }
        }
        *cached_page = page;
        *cached_base = (uint8_t *)phys_to_direct(physical);
    }
    if (*cached_base == 0) return false;
    *pixel = *(const uint32_t *)(const void *)(*cached_base +
                                               (virtual_address & (PAGE_SIZE - 1U)));
    return true;
}

static void compositor_surface_locked(const window_server_window_t *window) {
    uint64_t cached_page = UINT64_MAX;
    uint8_t *cached_base = 0;
    int64_t frame_width;
    int64_t frame_height;
    int64_t surface_x;
    int64_t surface_y;
    if (window == 0) return;
    frame_width = (int64_t)window->width + WINDOW_FRAME_EXTRA;
    frame_height = (int64_t)window->height + WINDOW_FRAME_EXTRA;
    surface_x = (int64_t)window->x + WINDOW_FRAME_BORDER;
    surface_y = (int64_t)window->y + WINDOW_FRAME_BORDER;
    for (uint32_t row = 0U; row < window->height; ++row) {
        int64_t destination_y = surface_y + row;
        int64_t frame_row = destination_y - window->y;
        int64_t first_column = 0;
        int64_t last_column = window->width;
        uint32_t inset;
        volatile uint32_t *destination;
        if (destination_y < (int64_t)g_window_server.damage_top ||
            destination_y >= (int64_t)g_window_server.damage_bottom) {
            continue;
        }
        inset = frame_row >= 0 && frame_row < frame_height ?
                compositor_corner_inset((uint32_t)frame_row, (uint32_t)frame_width,
                                        (uint32_t)frame_height) : 0U;
        if (surface_x + first_column < 0) {
            first_column = -surface_x;
        }
        if (surface_x + last_column > (int64_t)g_window_server.display_width) {
            last_column = (int64_t)g_window_server.display_width - surface_x;
        }
        if (surface_x + first_column < (int64_t)g_window_server.damage_left) {
            first_column = (int64_t)g_window_server.damage_left - surface_x;
        }
        if (surface_x + last_column > (int64_t)g_window_server.damage_right) {
            last_column = (int64_t)g_window_server.damage_right - surface_x;
        }
        /* surface 也要按圆角裁剪，但必须额外让出一像素边框；
         * 直接使用外框边界会把弧线位置的边框覆盖掉。 */
        if (first_column < (int64_t)inset) {
            first_column = inset;
        }
        if (last_column > (int64_t)window->width - inset) {
            last_column = (int64_t)window->width - inset;
        }
        if (first_column < 0) first_column = 0;
        if (last_column > (int64_t)window->width) last_column = window->width;
        if (first_column >= last_column) continue;
        destination = g_window_server.composite_framebuffer +
            (uint64_t)(uint32_t)destination_y * g_window_server.display_stride +
            (uint32_t)(surface_x + first_column);
        for (int64_t column = first_column; column < last_column; ++column) {
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
    }
}

static bool compositor_window_intersects_damage_locked(
    const window_server_window_t *window) {
    int64_t left;
    int64_t top;
    int64_t right;
    int64_t bottom;
    if (window == 0) return false;
    left = window->x;
    top = window->y;
    right = left + (int64_t)window->width + WINDOW_FRAME_EXTRA;
    bottom = top + (int64_t)window->height + WINDOW_FRAME_EXTRA;
    return left < (int64_t)g_window_server.damage_right &&
           right > (int64_t)g_window_server.damage_left &&
           top < (int64_t)g_window_server.damage_bottom &&
           bottom > (int64_t)g_window_server.damage_top;
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
    const int32_t origin_x = (int32_t)g_window_server.pointer_x -
                             (int32_t)WINDOW_CURSOR_HOTSPOT_X;
    const int32_t origin_y = (int32_t)g_window_server.pointer_y -
                             (int32_t)WINDOW_CURSOR_HOTSPOT_Y;
    for (uint32_t row = 0U; row < WINDOW_CURSOR_HEIGHT; ++row) {
        for (uint32_t column = 0U; column < WINDOW_CURSOR_WIDTH; ++column) {
            uint32_t pixel = g_linux_cursor_argb[
                row * WINDOW_CURSOR_WIDTH + column];
            uint32_t alpha = pixel >> 24;
            if (alpha == 0U) continue;
            compositor_blend_pixel_locked(
                origin_x + (int32_t)column, origin_y + (int32_t)row,
                pixel & 0x00FFFFFFU, alpha);
        }
    }
}


static void compositor_region_locked(void) {
    uint32_t damage_left = g_window_server.damage_left;
    uint32_t damage_top = g_window_server.damage_top;
    uint32_t damage_right = g_window_server.damage_right;
    uint32_t damage_bottom = g_window_server.damage_bottom;
    if (damage_left >= damage_right || damage_top >= damage_bottom) return;
    compositor_fill_locked((int32_t)damage_left, (int32_t)damage_top,
                           damage_right - damage_left,
                           damage_bottom - damage_top, 0x00101928U);
    for (uint32_t index = 0U; index < g_window_server.count; ++index) {
        window_server_window_t *window = g_window_server.windows[index];
        uint32_t frame_color;
        if (window == 0 || (window->flags & OS_WINDOW_VISIBLE) == 0U ||
            !compositor_window_intersects_damage_locked(window)) continue;
        frame_color = window->identifier == g_window_server.focused_identifier ?
                      0x005C99C6U : 0x002A3B4BU;
        compositor_fill_rounded_locked(window->x, window->y,
                                       window->width + WINDOW_FRAME_EXTRA,
                                       window->height + WINDOW_FRAME_EXTRA,
                                       WINDOW_CORNER_RADIUS, frame_color);
        compositor_surface_locked(window);
    }
    compositor_cursor_locked();
    if (g_window_server.composite_framebuffer != g_window_server.framebuffer) {
        for (uint32_t row = damage_top; row < damage_bottom; ++row) {
            volatile uint32_t *destination = g_window_server.framebuffer +
                (uint64_t)row * g_window_server.display_stride;
            const volatile uint32_t *source = g_window_server.composite_framebuffer +
                (uint64_t)row * g_window_server.display_stride;
            for (uint32_t column = damage_left; column < damage_right; ++column) {
                destination[column] = source[column];
            }
        }
        /* framebuffer 使用 WC 映射；必须在本次区域提交完成后冲刷
         * write-combining buffer，避免 QEMU 看到乱序/半完成的拖动帧。 */
        __asm__ volatile ("sfence" : : : "memory");
    }
}

static void compositor_locked(void) {
    uint32_t region_count;
    if (!g_window_server.kernel_ready || !g_window_server.dirty ||
        g_window_server.composing || g_window_server.framebuffer == 0) return;
    if (g_window_server.damage_full) {
        region_count = 1U;
    } else {
        region_count = g_window_server.damage_count;
    }
    if (region_count == 0U) {
        g_window_server.dirty = false;
        return;
    }
    g_window_server.composing = true;
    for (uint32_t index = 0U; index < region_count; ++index) {
        if (g_window_server.damage_full) {
            g_window_server.damage_left = 0U;
            g_window_server.damage_top = 0U;
            g_window_server.damage_right = g_window_server.display_width;
            g_window_server.damage_bottom = g_window_server.display_height;
        } else {
            window_damage_rect_t *rect = &g_window_server.damage_rects[index];
            g_window_server.damage_left = rect->left;
            g_window_server.damage_top = rect->top;
            g_window_server.damage_right = rect->right;
            g_window_server.damage_bottom = rect->bottom;
        }
        compositor_region_locked();
    }
    g_window_server.damage_count = 0U;
    g_window_server.damage_full = false;
    g_window_server.dirty = false;
    g_window_server.composing = false;
}

static void window_enqueue_event_locked(window_server_window_t *window,
                                         const input_event_t *event) {
    if (window == 0 || event == 0) return;
    if (window->event_count >= WINDOW_EVENT_CAPACITY) {
        window->event_read = (window->event_read + 1U) % WINDOW_EVENT_CAPACITY;
        --window->event_count;
    }
    window->events[window->event_write].identifier = window->identifier;
    window->events[window->event_write].reserved = 0U;
    window->events[window->event_write].input.timestamp = event->timestamp;
    window->events[window->event_write].input.device_id = event->device_id;
    window->events[window->event_write].input.type = event->type;
    window->events[window->event_write].input.flags = event->flags;
    window->events[window->event_write].input.code = event->code;
    window->events[window->event_write].input.value = event->value;
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
        (flags & ~OS_WINDOW_VISIBLE) != 0U) return K_EINVAL;
    pixels = (uint64_t)width * height;
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
    if (window->identifier == 0U) window->identifier = g_window_server.next_identifier++;
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
                                  window->width + WINDOW_FRAME_EXTRA,
                                  window->height + WINDOW_FRAME_EXTRA);
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
    window->events[window->event_write].reserved = 0U;
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
    /* x/y/width/height 是 surface 内的 damage；0,0,0,0 表示整窗。 */
    window->dirty = true;
    if (width == 0U) {
        window_mark_window_locked(window);
    } else {
        window_mark_surface_locked(window, x, y, width, height);
    }
    /* 只登记 damage；由 Ring0 主循环统一合成，避免每个窗口 syscall
     * 都独占窗口锁并重复扫描整个 z-order。 */
    window_unlock();
    return K_OK;
}

static void route_input_locked(const input_event_t *event) {
    window_server_window_t *target = 0;
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
            if (dragged != 0) {
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
            }
        }
        if (target == 0) target = window_at_locked(g_window_server.pointer_x,
                                                    g_window_server.pointer_y);
        window_mark_moved_cursor_locked(old_pointer_x, old_pointer_y,
                                        g_window_server.pointer_x,
                                        g_window_server.pointer_y);
    } else if (event->type == INPUT_EVENT_BUTTON) {
        target = window_at_locked(g_window_server.pointer_x,
                                  g_window_server.pointer_y);
        if (event->code == INPUT_BUTTON_LEFT &&
            event->value == INPUT_VALUE_PRESS && target != 0) {
            focus_locked(target);
            if ((int64_t)g_window_server.pointer_y - target->y <
                WINDOW_DRAG_REGION_HEIGHT) {
                g_window_server.dragging_identifier = target->identifier;
                g_window_server.drag_offset_x =
                    (int32_t)g_window_server.pointer_x - target->x;
                g_window_server.drag_offset_y =
                    (int32_t)g_window_server.pointer_y - target->y;
            }
        } else if (event->code == INPUT_BUTTON_LEFT &&
                   event->value == INPUT_VALUE_RELEASE) {
            g_window_server.dragging_identifier = 0U;
            g_window_server.drag_offset_x = 0;
            g_window_server.drag_offset_y = 0;
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
                    window_server_window_t *candidate = g_window_server.windows[index];
                    if (candidate != 0 && (candidate->flags & OS_WINDOW_VISIBLE) != 0U) {
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
    if (target != 0) window_enqueue_event_locked(target, event);
}

void window_server_pump_input(void) {
    input_event_t event;
    bool wake = false;
    if (!window_server_kernel_ready()) return;
    for (uint32_t count = 0U; count < WINDOW_EVENT_CAPACITY &&
         input_core_pop(&event) == K_OK; ++count) {
        window_lock();
        route_input_locked(&event);
        window_unlock();
        wake = true;
    }
    if (wake) (void)wake_all(&g_window_server.event_waitq);
    window_lock();
    compositor_locked();
    window_unlock();
}
