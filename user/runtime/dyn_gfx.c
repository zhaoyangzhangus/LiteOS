#include <stdbool.h>
#include <stdint.h>

#include <blend2d/blend2d.h>
#include <uapi/display.h>
#include <uapi/syscall.h>
#include <uapi/window.h>

#include "blend2d_demo.h"
#include "liteos_gfx.h"

#define DYNAMIC_GFX_MAP_BASE 0x07000000ULL
#define DYNAMIC_GFX_MIN_WIDTH 320U
#define DYNAMIC_GFX_MIN_HEIGHT 240U

extern int __libc_thread_init(void);

__attribute__((visibility("hidden"))) void *memset(void *destination, int value,
                                                     uint64_t length) {
    uint8_t *output = (uint8_t *)destination;
    while (length-- != 0U) *output++ = (uint8_t)value;
    return destination;
}

static int64_t dyn_gfx_syscall6(uint64_t number, uint64_t argument0,
                                uint64_t argument1, uint64_t argument2,
                                uint64_t argument3, uint64_t argument4,
                                uint64_t argument5) {
    register uint64_t r10 __asm__("r10") = argument3;
    register uint64_t r8 __asm__("r8") = argument4;
    register uint64_t r9 __asm__("r9") = argument5;
    uint64_t result;
    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"(number), "D"(argument0), "S"(argument1),
                       "d"(argument2), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return (int64_t)result;
}

static int64_t dyn_gfx_syscall1(uint64_t number, uint64_t argument0) {
    return dyn_gfx_syscall6(number, argument0, 0U, 0U, 0U, 0U, 0U);
}

static void dyn_gfx_debug(const char *message) {
    uint64_t length = 0U;
    if (message == 0) return;
    while (message[length] != '\0') ++length;
    (void)dyn_gfx_syscall6(OS_SYS_DEBUG_WRITE,
                           (uint64_t)(uintptr_t)message, length,
                           0U, 0U, 0U, 0U);
}

static bool create_window(const os_display_info_t *display,
                          os_window_create_t *window) {
    uint32_t width;
    uint32_t height;
    if (display == 0 || window == 0 || display->width < DYNAMIC_GFX_MIN_WIDTH ||
        display->height < DYNAMIC_GFX_MIN_HEIGHT) return false;
    width = display->width > 720U ? 640U : display->width - 48U;
    height = display->height > 480U ? 360U : display->height - 72U;
    if (width < DYNAMIC_GFX_MIN_WIDTH) width = DYNAMIC_GFX_MIN_WIDTH;
    if (height < DYNAMIC_GFX_MIN_HEIGHT) height = DYNAMIC_GFX_MIN_HEIGHT;
    window->hdr.size = sizeof(*window);
    window->hdr.version = OS_SYSCALL_ABI_VERSION;
    window->x = (int32_t)((display->width - width) / 2U);
    window->y = (int32_t)((display->height - height) / 2U);
    window->width = width;
    window->height = height;
    window->flags = OS_WINDOW_VISIBLE | OS_WINDOW_CLIENT_DECORATIONS;
    window->background = 0x00121C2EU;
    window->title[0] = 'D';
    window->title[1] = 'Y';
    window->title[2] = 'N';
    window->title[3] = 'A';
    window->title[4] = 'M';
    window->title[5] = 'I';
    window->title[6] = 'C';
    window->title[7] = ' ';
    window->title[8] = 'G';
    window->title[9] = 'F';
    window->title[10] = 'X';
    window->address = DYNAMIC_GFX_MAP_BASE;
    if (dyn_gfx_syscall1(OS_SYS_WINDOW_CREATE, (uint64_t)(uintptr_t)window) < 0) {
        return false;
    }
    if (window->identifier == 0U || window->address == 0U ||
        window->buffer_size < (uint64_t)window->width * window->height * 4U) {
        (void)dyn_gfx_syscall1(OS_SYS_HANDLE_CLOSE, window->window);
        window->window = OS_INVALID_HANDLE;
        return false;
    }
    return true;
}

static void draw_demo(uint32_t *pixels, uint32_t width, uint32_t height) {
    uint32_t panel_x = width / 10U;
    uint32_t panel_y = height / 8U;
    uint32_t panel_width = width - panel_x * 2U;
    uint32_t panel_height = height - panel_y * 2U;
    liteos_gfx_gradient_rect(pixels, width, width, height, 0, 0, width, height,
                             0x00101A30U, 0x003C1F62U);
    liteos_gfx_fill_rect(pixels, width, width, height,
                         (int32_t)panel_x, (int32_t)panel_y,
                         panel_width, panel_height, 0x001C2945U);
    liteos_gfx_gradient_rect(pixels, width, width, height,
                             (int32_t)panel_x + 4, (int32_t)panel_y + 4,
                             panel_width - 8U, panel_height / 2U,
                             0x002A6A92U, 0x00163B68U);
    liteos_gfx_fill_rect(pixels, width, width, height,
                         (int32_t)panel_x + panel_width / 8U,
                         (int32_t)panel_y + panel_height * 3U / 5U,
                         panel_width * 3U / 5U, 18U, 0x005DE0C0U);
    liteos_gfx_fill_rect(pixels, width, width, height,
                         (int32_t)panel_x + panel_width / 8U,
                         (int32_t)panel_y + panel_height * 3U / 5U + 30,
                         panel_width * 2U / 5U, 10U, 0x00E5B567U);
    liteos_gfx_fill_rect(pixels, width, width, height,
                         (int32_t)panel_x + panel_width * 5U / 8U,
                         (int32_t)panel_y + panel_height * 3U / 5U,
                         panel_width / 8U, 40U, 0x00F16D7AU);
    liteos_gfx_frame(pixels, width, width, height, 4U, 0x007DD6FFU);
}

static bool publish_window(uint32_t identifier, uint32_t width, uint32_t height) {
    os_window_update_t update = {0};
    update.hdr.size = sizeof(update);
    update.hdr.version = OS_SYSCALL_ABI_VERSION;
    update.identifier = identifier;
    update.width = width;
    update.height = height;
    return dyn_gfx_syscall1(OS_SYS_WINDOW_UPDATE, (uint64_t)(uintptr_t)&update) == 0;
}

static bool render_window(os_window_create_t *window, uint32_t font_size) {
    if (window == 0 || window->address == 0U) return false;
    draw_demo((uint32_t *)(uintptr_t)window->address,
              window->width, window->height);
    if (!liteos_blend2d_draw_demo((uint32_t *)(uintptr_t)window->address,
                                  window->width, window->width,
                                  window->height, font_size)) return false;
    return publish_window(window->identifier, window->width, window->height);
}

static bool change_font_size(uint32_t *font_size, int32_t direction) {
    uint32_t next;
    if (font_size == 0) return false;
    if (direction > 0) {
        next = *font_size >= LITEOS_BLEND2D_FONT_MAX - LITEOS_BLEND2D_FONT_STEP ?
               LITEOS_BLEND2D_FONT_MAX : *font_size + LITEOS_BLEND2D_FONT_STEP;
    } else {
        next = *font_size <= LITEOS_BLEND2D_FONT_MIN + LITEOS_BLEND2D_FONT_STEP ?
               LITEOS_BLEND2D_FONT_MIN : *font_size - LITEOS_BLEND2D_FONT_STEP;
    }
    if (next == *font_size) return false;
    *font_size = next;
    return true;
}

static bool handle_font_input(const os_window_event_t *event, bool *shift,
                              uint32_t *font_size) {
    const os_input_event_t *input;
    if (event == 0 || shift == 0 || font_size == 0 ||
        event->type != OS_WINDOW_EVENT_INPUT) return false;
    input = &event->input;
    if (input->type == OS_INPUT_EVENT_KEY) {
        if (input->code == 0xE1U || input->code == 0xE5U) {
            *shift = input->value != OS_INPUT_VALUE_RELEASE;
            return false;
        }
        if (input->value == OS_INPUT_VALUE_RELEASE) return false;
        if ((input->code == 0x2EU && *shift) || input->code == '+') {
            return change_font_size(font_size, 1);
        }
        if ((input->code == 0x2DU && !*shift) || input->code == '-') {
            return change_font_size(font_size, -1);
        }
    } else if (input->type == OS_INPUT_EVENT_RELATIVE &&
               input->code == OS_INPUT_REL_WHEEL && input->value != 0) {
        return change_font_size(font_size, input->value > 0 ? 1 : -1);
    }
    return false;
}

int64_t dyn_gfx_main(uint64_t argc, char **argv) {
    os_display_info_t display = {0};
    os_window_create_t window = {0};
    os_window_event_read_t event = {0};
    uint32_t font_size = LITEOS_BLEND2D_FONT_DEFAULT;
    bool shift = false;
    bool runtime_ready = false;
    bool window_ready = false;
    int64_t result = 1;
    (void)argc;
    (void)argv;
    if (__libc_thread_init() != 0) return 1;
    if (bl_runtime_init() != BL_SUCCESS) return 1;
    runtime_ready = true;
    display.hdr.size = sizeof(display);
    display.hdr.version = OS_SYSCALL_ABI_VERSION;
    if (dyn_gfx_syscall1(OS_SYS_DISPLAY_GET_INFO, (uint64_t)(uintptr_t)&display) < 0 ||
        display.format != OS_DISPLAY_FORMAT_XRGB8888 ||
        !create_window(&display, &window)) goto cleanup;
    window_ready = true;
    if (!render_window(&window, font_size)) {
        goto cleanup;
    }
    if (!publish_window(window.identifier, window.width, window.height)) {
        goto cleanup;
    }
    result = 0;
    dyn_gfx_debug("LITEOS_BLEND2D_VISIBLE_OK\r\n");
    for (;;) {
        event = (os_window_event_read_t){0};
        event.hdr.size = sizeof(event);
        event.hdr.version = OS_SYSCALL_ABI_VERSION;
        event.identifier = window.identifier;
        event.timeout_ns = OS_WAIT_INFINITE;
        if (dyn_gfx_syscall1(OS_SYS_WINDOW_EVENT_READ,
                             (uint64_t)(uintptr_t)&event) < 0) continue;
        if (event.event.type == OS_WINDOW_EVENT_CLOSE_REQUEST) break;
        if (handle_font_input(&event.event, &shift, &font_size)) {
            if (!render_window(&window, font_size)) {
                result = 1;
                break;
            }
            dyn_gfx_debug(font_size == LITEOS_BLEND2D_FONT_MAX ?
                          "LITEOS_BLEND2D_FONT_MAX\r\n" :
                          font_size == LITEOS_BLEND2D_FONT_MIN ?
                          "LITEOS_BLEND2D_FONT_MIN\r\n" :
                          "LITEOS_BLEND2D_FONT_ZOOM\r\n");
        }
    }

cleanup:
    if (window_ready) (void)dyn_gfx_syscall1(OS_SYS_HANDLE_CLOSE, window.window);
    if (runtime_ready) (void)bl_runtime_shutdown();
    return result;
}
