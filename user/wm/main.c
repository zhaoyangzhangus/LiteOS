#include <stdint.h>
#include <stdbool.h>

#include <uapi/all.h>

#define WM_MAX_WINDOWS       64U
#define WM_TITLEBAR_HEIGHT   24U
#define WM_TOPBAR_HEIGHT     36U
#define WM_TASKBAR_HEIGHT    38U
#define WM_MAP_BASE          0x20000000ULL
#define WM_MAP_SLOT_SIZE     0x02000000ULL
#define WM_FRAME_INTERVAL    16000000ULL
#define WM_INPUT_TIMEOUT     4000000ULL
#define WM_STATUS_NO_ENTRY   (-2LL)
#define WM_STATUS_AGAIN      (-11LL)
#define WM_STATUS_TIMEOUT    (-110LL)

typedef struct wm_surface {
    uint32_t identifier;
    uint64_t address;
    uint64_t size;
    bool mapped;
} wm_surface_t;

static uint32_t g_width;
static uint32_t g_height;
static uint32_t g_stride;
static uint32_t g_format;
static uint32_t *g_frame;
static os_handle_t g_gpu_allocation = OS_INVALID_HANDLE;
static os_window_info_t g_windows[WM_MAX_WINDOWS];
static wm_surface_t g_surfaces[WM_MAX_WINDOWS];
static uint32_t g_window_count;
static uint32_t g_cursor_x;
static uint32_t g_cursor_y;
static bool g_dragging;
static uint32_t g_drag_identifier;
static int32_t g_drag_offset_x;
static int32_t g_drag_offset_y;

static int64_t wm_syscall_one(uint64_t number, uint64_t arg0) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = arg0;
    __asm__ volatile ("syscall" : "+a"(rax), "+D"(rdi) :
                      : "rcx", "r11", "memory");
    return (int64_t)rax;
}

static int64_t wm_syscall_two(uint64_t number, uint64_t arg0, uint64_t arg1) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = arg0;
    register uint64_t rsi __asm__("rsi") = arg1;
    __asm__ volatile ("syscall" : "+a"(rax), "+D"(rdi), "+S"(rsi) :
                      : "rcx", "r11", "memory");
    return (int64_t)rax;
}

static void wm_exit(uint64_t status) {
    (void)wm_syscall_one(OS_SYS_THREAD_EXIT, status);
    for (;;) __asm__ volatile ("pause");
}

static uint64_t monotonic_time_ns(void) {
    os_timespec_t value = {0};
    if (wm_syscall_two(OS_SYS_CLOCK_GET, 0U, (uint64_t)&value) != 0 ||
        value.seconds < 0 || value.nanoseconds < 0) return 0U;
    if ((uint64_t)value.seconds > UINT64_MAX / 1000000000ULL) return 0U;
    return (uint64_t)value.seconds * 1000000000ULL +
           (uint64_t)value.nanoseconds;
}

static void fill_rect(int32_t x, int32_t y, uint32_t width, uint32_t height,
                      uint32_t color) {
    int32_t left = x < 0 ? 0 : x;
    int32_t top = y < 0 ? 0 : y;
    int64_t right = (int64_t)x + width;
    int64_t bottom = (int64_t)y + height;
    if (right > (int64_t)g_width) right = g_width;
    if (bottom > (int64_t)g_height) bottom = g_height;
    if (left >= right || top >= bottom) return;
    for (int32_t row = top; row < bottom; ++row) {
        for (int32_t column = left; column < right; ++column) {
            g_frame[(uint64_t)row * g_stride + (uint32_t)column] = color;
        }
    }
}

static const uint8_t *glyph_for(char character) {
    static const uint8_t font[37][7] = {
        {0,0,0,0,0,0,0},
        {30,9,9,30,9,9,0}, {30,9,9,30,9,30,0}, {14,17,16,16,17,14,0},
        {28,10,9,9,10,28,0}, {31,16,16,30,16,31,0}, {31,16,16,30,16,16,0},
        {14,17,16,23,17,15,0}, {17,17,17,31,17,17,0}, {14,4,4,4,4,14,0},
        {7,2,2,2,18,12,0}, {17,18,20,24,20,18,0}, {16,16,16,16,16,31,0},
        {17,27,21,17,17,17,0}, {17,25,21,19,17,17,0}, {14,17,17,17,17,14,0},
        {30,17,17,30,16,16,0}, {14,17,17,17,21,14,1}, {30,17,17,30,20,18,0},
        {15,16,16,14,1,30,0}, {31,4,4,4,4,4,0}, {17,17,17,17,17,14,0},
        {17,17,17,17,10,4,0}, {17,17,17,21,27,17,0}, {17,10,4,10,17,17,0},
        {17,10,4,4,4,4,0}, {31,2,4,8,16,31,0},
        {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14}, {14,17,1,6,8,16,31},
        {30,1,1,14,1,17,14}, {2,6,10,18,31,2,2}, {31,16,16,30,1,17,14},
        {6,8,16,30,17,17,14}, {31,1,2,4,8,8,8}, {14,17,17,14,17,17,14},
        {14,17,17,15,1,2,12},
    };
    if (character == ' ') return font[0];
    if (character >= 'A' && character <= 'Z') return font[1U + (uint32_t)(character - 'A')];
    if (character >= '0' && character <= '9') return font[27U + (uint32_t)(character - '0')];
    return font[0];
}

static void draw_text(int32_t x, int32_t y, const char *text, uint32_t color) {
    for (uint32_t index = 0U; text != 0 && text[index] != '\0'; ++index) {
        const uint8_t *glyph = glyph_for(text[index]);
        for (uint32_t row = 0U; row < 7U; ++row) {
            for (uint32_t column = 0U; column < 5U; ++column) {
                if ((glyph[row] & (1U << (4U - column))) != 0U) {
                    fill_rect(x + (int32_t)index * 6 + (int32_t)column,
                              y + (int32_t)row, 1U, 1U, color);
                }
            }
        }
    }
}

static wm_surface_t *surface_for(uint32_t identifier) {
    for (uint32_t i = 0U; i < WM_MAX_WINDOWS; ++i) {
        if (g_surfaces[i].mapped && g_surfaces[i].identifier == identifier) {
            return &g_surfaces[i];
        }
    }
    return 0;
}

static bool enumerate_windows(void) {
    g_window_count = 0U;
    for (uint32_t index = 0U; index < WM_MAX_WINDOWS; ++index) {
        os_window_enumerate_t request = {0};
        int64_t status;
        request.hdr.size = sizeof(request);
        request.hdr.version = OS_SYSCALL_ABI_VERSION;
        request.index = index;
        status = wm_syscall_one(OS_SYS_WINDOW_ENUMERATE, (uint64_t)&request);
        if (status == WM_STATUS_NO_ENTRY) break;
        if (status != 0) return false;
        g_windows[g_window_count++] = request.info;
    }
    return true;
}

static bool ensure_surface(const os_window_info_t *info, uint32_t slot) {
    wm_surface_t *surface = surface_for(info->identifier);
    if (surface != 0) return true;
    if (slot >= WM_MAX_WINDOWS) return false;
    os_window_map_t request = {0};
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.identifier = info->identifier;
    request.address = WM_MAP_BASE + (uint64_t)slot * WM_MAP_SLOT_SIZE;
    request.length = info->buffer_size;
    if (wm_syscall_one(OS_SYS_WINDOW_MAP, (uint64_t)&request) != 0) return false;
    g_surfaces[slot].identifier = info->identifier;
    g_surfaces[slot].address = request.address;
    g_surfaces[slot].size = request.length;
    g_surfaces[slot].mapped = true;
    return true;
}

static int32_t clamp_cursor(int64_t value, uint32_t extent) {
    if (extent == 0U || value <= 0) return 0;
    if ((uint64_t)value >= extent) return (int32_t)(extent - 1U);
    return (int32_t)value;
}

static int32_t window_at(uint32_t x, uint32_t y) {
    for (uint32_t index = g_window_count; index != 0U; --index) {
        const os_window_info_t *info = &g_windows[index - 1U];
        int64_t right = (int64_t)info->x + info->width + 4U;
        int64_t bottom = (int64_t)info->y + info->height + WM_TITLEBAR_HEIGHT + 4U;
        if (info->visible != 0U && (int64_t)x >= info->x && (int64_t)y >= info->y &&
            (int64_t)x < right && (int64_t)y < bottom) return (int32_t)(index - 1U);
    }
    return -1;
}

static os_window_info_t *window_by_id(uint32_t identifier) {
    for (uint32_t i = 0U; i < g_window_count; ++i) {
        if (g_windows[i].identifier == identifier) return &g_windows[i];
    }
    return 0;
}

static bool set_focus(uint32_t identifier) {
    os_window_focus_t request = {0};
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.identifier = identifier;
    return wm_syscall_one(OS_SYS_WINDOW_FOCUS, (uint64_t)&request) == 0;
}

static bool move_window(uint32_t identifier, int32_t x, int32_t y) {
    os_window_set_t request = {0};
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.identifier = identifier;
    request.visible = 1U;
    request.x = x;
    request.y = y;
    return wm_syscall_one(OS_SYS_WINDOW_SET, (uint64_t)&request) == 0;
}

static void dispatch_to_focus(const os_input_event_t *event) {
    uint32_t identifier = 0U;
    for (uint32_t i = 0U; i < g_window_count; ++i) {
        if (g_windows[i].focused != 0U) {
            identifier = g_windows[i].identifier;
            break;
        }
    }
    if (identifier == 0U || event == 0) return;
    os_window_input_dispatch_t request = {0};
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.identifier = identifier;
    request.event = *event;
    (void)wm_syscall_one(OS_SYS_WINDOW_INPUT_DISPATCH, (uint64_t)&request);
}

static bool handle_input(const os_input_event_t *event) {
    if (event == 0) return false;
    if (event->type == OS_INPUT_EVENT_RELATIVE) {
        if (event->code == OS_INPUT_REL_X) {
            g_cursor_x = (uint32_t)clamp_cursor((int64_t)g_cursor_x + event->value,
                                                g_width);
        } else if (event->code == OS_INPUT_REL_Y) {
            g_cursor_y = (uint32_t)clamp_cursor((int64_t)g_cursor_y + event->value,
                                                g_height);
        }
        if (g_dragging) {
            os_window_info_t *info = window_by_id(g_drag_identifier);
            if (info != 0) {
                (void)move_window(g_drag_identifier,
                                  (int32_t)g_cursor_x - g_drag_offset_x,
                                  (int32_t)g_cursor_y - g_drag_offset_y);
            }
        }
        return true;
    }
    if (event->type == OS_INPUT_EVENT_BUTTON &&
        event->code == OS_INPUT_BUTTON_LEFT) {
        if (event->value == OS_INPUT_VALUE_RELEASE) {
            g_dragging = false;
            g_drag_identifier = 0U;
            return true;
        }
        if (event->value == OS_INPUT_VALUE_PRESS) {
            int32_t index = window_at(g_cursor_x, g_cursor_y);
            if (index >= 0) {
                os_window_info_t *info = &g_windows[(uint32_t)index];
                (void)set_focus(info->identifier);
                if ((int64_t)g_cursor_y - info->y < WM_TITLEBAR_HEIGHT) {
                    g_dragging = true;
                    g_drag_identifier = info->identifier;
                    g_drag_offset_x = (int32_t)g_cursor_x - info->x;
                    g_drag_offset_y = (int32_t)g_cursor_y - info->y;
                }
            }
            return true;
        }
    }
    if (event->type == OS_INPUT_EVENT_KEY) {
        if (event->value != OS_INPUT_VALUE_RELEASE && event->code == 0x2BU) {
            uint32_t current = 0U;
            for (uint32_t i = 0U; i < g_window_count; ++i) {
                if (g_windows[i].focused != 0U) current = i;
            }
            if (g_window_count != 0U) {
                (void)set_focus(g_windows[(current + 1U) % g_window_count].identifier);
            }
            return true;
        }
        dispatch_to_focus(event);
        return false;
    }
    return false;
}

static void draw_cursor(void) {
    for (uint32_t row = 0U; row < 12U; ++row) {
        fill_rect((int32_t)g_cursor_x, (int32_t)g_cursor_y + (int32_t)row,
                  2U + row / 2U, 1U, 0x0006090CU);
    }
    fill_rect((int32_t)g_cursor_x + 1, (int32_t)g_cursor_y + 1, 1U, 9U,
              0x00F5FBFFU);
}

static void copy_surface(const os_window_info_t *info) {
    wm_surface_t *surface = surface_for(info->identifier);
    uint32_t *source;
    if (surface == 0) return;
    source = (uint32_t *)(uintptr_t)surface->address;
    for (uint32_t row = 0U; row < info->height; ++row) {
        int32_t destination_y = info->y + (int32_t)WM_TITLEBAR_HEIGHT + 2 +
                                (int32_t)row;
        if (destination_y < 0 || destination_y >= (int32_t)g_height) continue;
        for (uint32_t column = 0U; column < info->width; ++column) {
            int32_t destination_x = info->x + 2 + (int32_t)column;
            if (destination_x < 0 || destination_x >= (int32_t)g_width) continue;
            g_frame[(uint64_t)destination_y * g_stride + (uint32_t)destination_x] =
                source[(uint64_t)row * info->width + column];
        }
    }
}

static void render_frame(void) {
    fill_rect(0, 0, g_width, g_height, 0x00101928U);
    fill_rect(0, 0, g_width, WM_TOPBAR_HEIGHT, 0x00213D5AU);
    draw_text(20, 12, "LITEOS WINDOW SERVER", 0x00E8F1F5U);
    for (uint32_t index = 0U; index < g_window_count; ++index) {
        const os_window_info_t *info = &g_windows[index];
        uint32_t frame_color = info->focused != 0U ? 0x005C99C6U : 0x002A3B4BU;
        fill_rect(info->x, info->y, info->width + 4U,
                  info->height + WM_TITLEBAR_HEIGHT + 4U, frame_color);
        fill_rect(info->x + 2, info->y + 2, info->width,
                  WM_TITLEBAR_HEIGHT, info->focused != 0U ? 0x002A6691U : 0x0022394FU);
        draw_text(info->x + 8, info->y + 9, info->title,
                  info->focused != 0U ? 0x00F3FAFFU : 0x00B5C8D8U);
        copy_surface(info);
    }
    fill_rect(0, (int32_t)g_height - WM_TASKBAR_HEIGHT, g_width,
              WM_TASKBAR_HEIGHT, 0x00162538U);
    if (g_window_count != 0U) {
        uint32_t button_width = g_width / g_window_count;
        for (uint32_t index = 0U; index < g_window_count; ++index) {
            int32_t x = (int32_t)(index * button_width + 6U);
            fill_rect(x, (int32_t)g_height - WM_TASKBAR_HEIGHT + 7,
                      button_width > 12U ? button_width - 12U : button_width,
                      WM_TASKBAR_HEIGHT - 14U,
                      g_windows[index].focused != 0U ? 0x003B7198U : 0x001C2D40U);
            draw_text(x + 8, (int32_t)g_height - WM_TASKBAR_HEIGHT + 14,
                      g_windows[index].title, 0x00E8F1F5U);
        }
    }
    draw_cursor();
}

static bool setup_graphics(void) {
    os_display_info_t info = {0};
    os_gpu_create_context_t context = {0};
    os_gpu_alloc_t allocation = {0};
    os_gpu_map_t map = {0};
    os_handle_t context_handle = OS_INVALID_HANDLE;
    os_handle_t allocation_handle = OS_INVALID_HANDLE;
    uint64_t bytes;
    info.hdr.size = sizeof(info);
    info.hdr.version = OS_SYSCALL_ABI_VERSION;
    if (wm_syscall_one(OS_SYS_DISPLAY_GET_INFO, (uint64_t)&info) < 0 ||
        info.width == 0U || info.height == 0U || info.stride < info.width ||
        info.format != OS_DISPLAY_FORMAT_XRGB8888) return false;
    bytes = (uint64_t)info.stride * info.height * sizeof(uint32_t);
    bytes = (bytes + 4095U) & ~4095ULL;
    context.hdr.size = sizeof(context);
    context.hdr.version = OS_SYSCALL_ABI_VERSION;
    context.device = OS_INVALID_HANDLE;
    if (wm_syscall_two(OS_SYS_GPU_CREATE_CTX, (uint64_t)&context,
                       (uint64_t)&context_handle) < 0) return false;
    allocation.hdr.size = sizeof(allocation);
    allocation.hdr.version = OS_SYSCALL_ABI_VERSION;
    allocation.size = bytes;
    if (wm_syscall_two(OS_SYS_GPU_ALLOC, (uint64_t)&allocation,
                       (uint64_t)&allocation_handle) < 0) return false;
    g_gpu_allocation = allocation_handle;
    (void)wm_syscall_one(OS_SYS_HANDLE_CLOSE, context_handle);
    map.hdr.size = sizeof(map);
    map.hdr.version = OS_SYSCALL_ABI_VERSION;
    map.allocation = g_gpu_allocation;
    map.address = 0x08000000ULL;
    map.length = bytes;
    map.prot = OS_VM_READ | OS_VM_WRITE;
    map.flags = OS_GPU_MAP_FIXED;
    if (wm_syscall_one(OS_SYS_GPU_MAP, (uint64_t)&map) < 0) return false;
    g_width = info.width;
    g_height = info.height;
    g_stride = info.stride;
    g_format = info.format;
    g_frame = (uint32_t *)(uintptr_t)map.address;
    return true;
}

static bool commit_frame(void) {
    os_display_commit_t commit = {0};
    int64_t status;
    commit.hdr.size = sizeof(commit);
    commit.hdr.version = OS_SYSCALL_ABI_VERSION;
    commit.buffer = g_gpu_allocation;
    commit.stride = (uint64_t)g_stride * sizeof(uint32_t);
    commit.width = g_width;
    commit.height = g_height;
    commit.format = g_format;
    commit.signal_value = 1U;
    status = wm_syscall_one(OS_SYS_DISPLAY_COMMIT, (uint64_t)&commit);
    if (commit.signal_fence != OS_INVALID_HANDLE) {
        (void)wm_syscall_one(OS_SYS_HANDLE_CLOSE, commit.signal_fence);
    }
    return status == 0;
}

__attribute__((noreturn)) void wm_entry(void) {
    os_input_event_t event;
    uint64_t last_frame = 0U;
    if (!setup_graphics() ||
        wm_syscall_one(OS_SYS_WINDOW_REGISTER_MANAGER, 0U) != 0) wm_exit(1U);
    g_cursor_x = g_width / 2U;
    g_cursor_y = g_height / 2U;
    for (;;) {
        uint64_t now = monotonic_time_ns();
        int64_t status = wm_syscall_two(OS_SYS_WINDOW_INPUT_READ,
                                        (uint64_t)&event, WM_INPUT_TIMEOUT);
        if (status == 0) (void)handle_input(&event);
        if (!enumerate_windows()) {
            wm_exit(2U);
        }
        for (uint32_t index = 0U; index < g_window_count; ++index) {
            if (!ensure_surface(&g_windows[index], index)) continue;
        }
        if (last_frame == 0U || now == 0U || now - last_frame >= WM_FRAME_INTERVAL) {
            render_frame();
            if (commit_frame()) last_frame = now;
        }
        if (status != 0 && status != WM_STATUS_AGAIN && status != WM_STATUS_TIMEOUT) {
            /* 输入设备短暂断开时仍让 compositor 保持可运行。 */
            last_frame = 0U;
        }
    }
}
