#include <stdint.h>
#include <stdbool.h>

#include <fcntl.h>
#include <unistd.h>
#include <liteos/libc.h>
#include <uapi/all.h>

#include "../../runtime/liteos_text.h"
#include "../../client_chrome.h"

#define IMAGEVIEW_DEFAULT_PATH  "/etc/desktop/wall.png"
#define IMAGEVIEW_PATH_CAPACITY 256U
#define IMAGEVIEW_MAP_BASE      0x08000000ULL
#define IMAGEVIEW_EVENT_WAIT OS_WAIT_INFINITE
#define IMAGEVIEW_STATUS_HEIGHT 32U
#define IMAGEVIEW_MIN_WIDTH     640U
#define IMAGEVIEW_MIN_HEIGHT    420U

#define IMAGEVIEW_BACKGROUND     0x001B2530U
#define IMAGEVIEW_CHECKER_LIGHT  0x00232F3BU
#define IMAGEVIEW_CHECKER_DARK   0x001E2934U
#define IMAGEVIEW_STATUS_TEXT    0x00545D66U

typedef struct imageview_window {
    uint32_t identifier;
    uint32_t width;
    uint32_t height;
    uint32_t *pixels;
} imageview_window_t;

static imageview_window_t g_window = {0};
static char g_path[IMAGEVIEW_PATH_CAPACITY];
static uint8_t *g_image_pixels;
static uint32_t g_image_width;
static uint32_t g_image_height;
static uint32_t g_image_stride;
static uint32_t g_zoom_percent;
static int32_t g_pan_x;
static int32_t g_pan_y;
static bool g_shift;
static bool g_ctrl;
static bool g_running = true;

static void copy_path(const char *source) {
    uint32_t index = 0U;

    if (source == 0) return;
    while (index + 1U < IMAGEVIEW_PATH_CAPACITY && source[index] != '\0') {
        g_path[index] = source[index];
        ++index;
    }
    g_path[index] = '\0';
}

static void imageview_cleanup(void) {
    if (g_image_pixels != 0) {
        free(g_image_pixels);
        g_image_pixels = 0;
    }
    liteos_text_shutdown();
}

static bool load_image_file(void) {
    int descriptor;
    off_t file_size;
    uint8_t *encoded;
    uint64_t remaining;

    descriptor = open(g_path, O_RDONLY);
    if (descriptor < 0) return false;
    file_size = lseek(descriptor, 0, SEEK_END);
    if (file_size <= 0 || (uint64_t)file_size > OS_IMAGE_MAX_ENCODED_BYTES ||
        lseek(descriptor, 0, SEEK_SET) < 0) {
        (void)close(descriptor);
        return false;
    }
    encoded = (uint8_t *)malloc((size_t)file_size);
    if (encoded == 0) {
        (void)close(descriptor);
        return false;
    }
    remaining = (uint64_t)file_size;
    while (remaining != 0U) {
        ssize_t count = read(descriptor, encoded + ((uint64_t)file_size - remaining),
                             (size_t)remaining);
        if (count <= 0 || (uint64_t)count > remaining) {
            free(encoded);
            (void)close(descriptor);
            return false;
        }
        remaining -= (uint64_t)count;
    }
    (void)close(descriptor);

    os_image_info_t info = {0};
    info.hdr.size = sizeof(info);
    info.hdr.version = OS_SYSCALL_ABI_VERSION;
    info.encoded = (uint64_t)(uintptr_t)encoded;
    info.encoded_size = (uint64_t)file_size;
    if (liteos_syscall6(OS_SYS_IMAGE_INFO, (uint64_t)(uintptr_t)&info,
                        0U, 0U, 0U, 0U, 0U) != 0 ||
        info.format != OS_IMAGE_PIXEL_RGBA8888 || info.width == 0U ||
        info.height == 0U || info.stride != info.width * 4U ||
        info.pixel_bytes == 0U ||
        info.pixel_bytes > OS_IMAGE_MAX_PIXEL_BYTES) {
        free(encoded);
        return false;
    }

    g_image_pixels = (uint8_t *)malloc((size_t)info.pixel_bytes);
    if (g_image_pixels == 0) {
        free(encoded);
        return false;
    }
    os_image_decode_t decode = {0};
    decode.hdr.size = sizeof(decode);
    decode.hdr.version = OS_SYSCALL_ABI_VERSION;
    decode.encoded = (uint64_t)(uintptr_t)encoded;
    decode.encoded_size = (uint64_t)file_size;
    decode.pixels = (uint64_t)(uintptr_t)g_image_pixels;
    decode.capacity = info.pixel_bytes;
    if (liteos_syscall6(OS_SYS_IMAGE_DECODE, (uint64_t)(uintptr_t)&decode,
                        0U, 0U, 0U, 0U, 0U) != 0 ||
        decode.format != OS_IMAGE_PIXEL_RGBA8888 ||
        decode.width != info.width || decode.height != info.height ||
        decode.stride != info.stride || decode.bytes_written != info.pixel_bytes) {
        free(encoded);
        imageview_cleanup();
        return false;
    }
    free(encoded);
    g_image_width = decode.width;
    g_image_height = decode.height;
    g_image_stride = decode.stride;
    return true;
}

static void fill_rect(uint32_t x, uint32_t y, uint32_t width,
                      uint32_t height, uint32_t color) {
    if (g_window.pixels == 0 || x >= g_window.width || y >= g_window.height ||
        width == 0U || height == 0U) return;
    if (width > g_window.width - x) width = g_window.width - x;
    if (height > g_window.height - y) height = g_window.height - y;
    for (uint32_t row = 0U; row < height; ++row) {
        uint32_t *destination = g_window.pixels +
            (uint64_t)(y + row) * g_window.width + x;
        for (uint32_t column = 0U; column < width; ++column) {
            destination[column] = color;
        }
    }
}

static uint32_t blend_pixel(uint32_t destination, uint32_t source,
                            uint8_t alpha) {
    uint32_t inverse = 255U - alpha;
    uint32_t red = (((source >> 16U) & 0xFFU) * alpha +
                    ((destination >> 16U) & 0xFFU) * inverse + 127U) / 255U;
    uint32_t green = (((source >> 8U) & 0xFFU) * alpha +
                      ((destination >> 8U) & 0xFFU) * inverse + 127U) / 255U;
    uint32_t blue = ((source & 0xFFU) * alpha +
                     (destination & 0xFFU) * inverse + 127U) / 255U;
    return (destination & 0xFF000000U) | (red << 16U) |
           (green << 8U) | blue;
}

static void draw_text(int32_t x, int32_t y, const char *text, uint32_t color) {
    liteos_text_draw(g_window.pixels, g_window.width,
                     g_window.width, g_window.height,
                     x, y, text, color);
}

static void draw_checkerboard(uint32_t top, uint32_t bottom) {
    const uint32_t tile = 16U;

    for (uint32_t y = top; y < bottom; y += tile) {
        for (uint32_t x = 0U; x < g_window.width; x += tile) {
            uint32_t color = ((x / tile + y / tile) & 1U) == 0U ?
                IMAGEVIEW_CHECKER_LIGHT : IMAGEVIEW_CHECKER_DARK;
            fill_rect(x, y, tile, tile, color);
        }
    }
}

static void clamp_pan(uint32_t content_width, uint32_t content_height) {
    uint32_t draw_width = (uint32_t)(((uint64_t)g_image_width *
                                      g_zoom_percent) / 100U);
    uint32_t draw_height = (uint32_t)(((uint64_t)g_image_height *
                                       g_zoom_percent) / 100U);
    int32_t limit_x = draw_width > content_width ?
        (int32_t)((draw_width - content_width) / 2U) : 0;
    int32_t limit_y = draw_height > content_height ?
        (int32_t)((draw_height - content_height) / 2U) : 0;

    if (g_pan_x < -limit_x) g_pan_x = -limit_x;
    if (g_pan_x > limit_x) g_pan_x = limit_x;
    if (g_pan_y < -limit_y) g_pan_y = -limit_y;
    if (g_pan_y > limit_y) g_pan_y = limit_y;
}

static uint32_t fit_zoom(uint32_t content_width, uint32_t content_height) {
    uint64_t horizontal;
    uint64_t vertical;
    uint64_t result;

    if (g_image_width == 0U || g_image_height == 0U ||
        content_width == 0U || content_height == 0U) return 1U;
    horizontal = (uint64_t)content_width * 100U / g_image_width;
    vertical = (uint64_t)content_height * 100U / g_image_height;
    result = horizontal < vertical ? horizontal : vertical;
    if (result == 0U) result = 1U;
    if (result > 100U) result = 100U;
    return (uint32_t)result;
}

static void reset_view(void) {
    uint32_t content_height = g_window.height >
        USER_CLIENT_CHROME_HEIGHT + IMAGEVIEW_STATUS_HEIGHT ?
        g_window.height - USER_CLIENT_CHROME_HEIGHT - IMAGEVIEW_STATUS_HEIGHT : 1U;
    g_zoom_percent = fit_zoom(g_window.width, content_height);
    g_pan_x = 0;
    g_pan_y = 0;
}

static void change_zoom(int32_t delta) {
    int32_t next = (int32_t)g_zoom_percent + delta;
    if (next < 10) next = 10;
    if (next > 400) next = 400;
    g_zoom_percent = (uint32_t)next;
    clamp_pan(g_window.width,
              g_window.height > USER_CLIENT_CHROME_HEIGHT +
                  IMAGEVIEW_STATUS_HEIGHT ?
                  g_window.height - USER_CLIENT_CHROME_HEIGHT -
                      IMAGEVIEW_STATUS_HEIGHT : 1U);
}

static void draw_image(uint32_t top, uint32_t bottom) {
    uint32_t content_height = bottom > top ? bottom - top : 0U;
    uint32_t draw_width;
    uint32_t draw_height;
    int64_t left;
    int64_t image_top;
    int64_t right;
    int64_t image_bottom;

    if (g_image_pixels == 0 || content_height == 0U ||
        g_zoom_percent == 0U) return;
    clamp_pan(g_window.width, content_height);
    draw_width = (uint32_t)(((uint64_t)g_image_width * g_zoom_percent) / 100U);
    draw_height = (uint32_t)(((uint64_t)g_image_height * g_zoom_percent) / 100U);
    if (draw_width == 0U || draw_height == 0U) return;
    left = ((int64_t)g_window.width - draw_width) / 2 + g_pan_x;
    image_top = (int64_t)top + ((int64_t)content_height - draw_height) / 2 + g_pan_y;
    right = left + draw_width;
    image_bottom = image_top + draw_height;

    for (int64_t y = image_top > (int64_t)top ? image_top : top;
         y < image_bottom && y < (int64_t)bottom; ++y) {
        uint32_t source_y = (uint32_t)(((uint64_t)(y - image_top) * 100U) /
                                       g_zoom_percent);
        if (source_y >= g_image_height) continue;
        for (int64_t x = left > 0 ? left : 0;
             x < right && x < (int64_t)g_window.width; ++x) {
            uint32_t source_x = (uint32_t)(((uint64_t)(x - left) * 100U) /
                                           g_zoom_percent);
            const uint8_t *source;
            uint32_t color;
            if (source_x >= g_image_width) continue;
            source = g_image_pixels + (uint64_t)source_y * g_image_stride +
                     (uint64_t)source_x * 4U;
            color = ((uint32_t)source[0] << 16) |
                    ((uint32_t)source[1] << 8) | source[2];
            if (source[3] == 255U) {
                g_window.pixels[(uint64_t)(uint32_t)y * g_window.width +
                                (uint32_t)x] = color;
            } else if (source[3] != 0U) {
                uint32_t *destination = &g_window.pixels[
                    (uint64_t)(uint32_t)y * g_window.width + (uint32_t)x];
                *destination = blend_pixel(*destination, color, source[3]);
            }
        }
    }
}

static void render(void) {
    char status[160];
    uint32_t content_top = USER_CLIENT_CHROME_HEIGHT;
    uint32_t content_bottom = g_window.height > IMAGEVIEW_STATUS_HEIGHT ?
        g_window.height - IMAGEVIEW_STATUS_HEIGHT : g_window.height;

    if (g_window.pixels == 0 || g_window.height == 0U) return;
    fill_rect(0U, 0U, g_window.width, g_window.height, IMAGEVIEW_BACKGROUND);
    fill_rect(0U, 0U, g_window.width, USER_CLIENT_CHROME_HEIGHT,
              USER_CLIENT_CHROME_BACKGROUND);
    fill_rect(0U, USER_CLIENT_CHROME_HEIGHT - 1U, g_window.width, 1U,
              USER_CLIENT_CHROME_SEPARATOR);
    user_client_chrome_app_icon(g_window.pixels, g_window.width,
                                g_window.width, g_window.height,
                                16U, USER_CLIENT_CHROME_TITLE_Y,
                                USER_CLIENT_CHROME_ICON_IMAGE);
    draw_text(50, USER_CLIENT_CHROME_TITLE_Y, "IMAGE VIEWER",
              USER_CLIENT_CHROME_TEXT);
    draw_text(210, USER_CLIENT_CHROME_TITLE_Y, g_path,
              USER_CLIENT_CHROME_TEXT);
    if (content_bottom > content_top) {
        draw_checkerboard(content_top, content_bottom);
        draw_image(content_top, content_bottom);
    }
    fill_rect(0U, content_bottom, g_window.width,
              g_window.height - content_bottom, USER_CLIENT_CHROME_CARD_BORDER);
    (void)snprintf(status, sizeof(status),
                    "%ux%u  %u%%   +/- IMAGE  CTRL +/- FONT  Q CLOSE",
                    g_image_width, g_image_height, g_zoom_percent);
    draw_text(10, (int32_t)content_bottom - 28, status, IMAGEVIEW_STATUS_TEXT);
    user_client_chrome_close(g_window.pixels, g_window.width,
                             g_window.width, g_window.height,
                             USER_CLIENT_CHROME_HEIGHT,
                             USER_CLIENT_CHROME_CLOSE_BG,
                             USER_CLIENT_CHROME_CLOSE_FG);
    user_client_chrome_frame(g_window.pixels, g_window.width,
                             g_window.width, g_window.height,
                             USER_CLIENT_CHROME_HEIGHT);

    os_window_update_t update = {0};
    update.hdr.size = sizeof(update);
    update.hdr.version = OS_SYSCALL_ABI_VERSION;
    update.identifier = g_window.identifier;
    update.width = g_window.width;
    update.height = g_window.height;
    (void)liteos_syscall6(OS_SYS_WINDOW_UPDATE, (uint64_t)(uintptr_t)&update,
                          0U, 0U, 0U, 0U, 0U);
}

static bool create_window(void) {
    os_display_info_t display = {0};
    os_window_create_t request = {0};
    uint32_t width;
    uint32_t height;

    display.hdr.size = sizeof(display);
    display.hdr.version = OS_SYSCALL_ABI_VERSION;
    if (liteos_syscall6(OS_SYS_DISPLAY_GET_INFO, (uint64_t)(uintptr_t)&display,
                        0U, 0U, 0U, 0U, 0U) < 0 || display.width < 320U ||
        display.height < 240U) return false;
    width = display.width * 3U / 4U;
    height = display.height * 3U / 4U;
    if (width < IMAGEVIEW_MIN_WIDTH && display.width > IMAGEVIEW_MIN_WIDTH) {
        width = IMAGEVIEW_MIN_WIDTH;
    }
    if (height < IMAGEVIEW_MIN_HEIGHT && display.height > IMAGEVIEW_MIN_HEIGHT) {
        height = IMAGEVIEW_MIN_HEIGHT;
    }
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.x = (int32_t)((display.width - width) / 2U);
    request.y = (int32_t)((display.height - height) / 2U);
    request.width = width;
    request.height = height;
    request.flags = OS_WINDOW_VISIBLE | OS_WINDOW_RESIZABLE |
                    OS_WINDOW_CLIENT_DECORATIONS;
    request.background = IMAGEVIEW_BACKGROUND;
    memcpy(request.title, "IMAGE VIEWER", sizeof("IMAGE VIEWER"));
    request.address = IMAGEVIEW_MAP_BASE;
    if (liteos_syscall6(OS_SYS_WINDOW_CREATE, (uint64_t)(uintptr_t)&request,
                        0U, 0U, 0U, 0U, 0U) != 0 ||
        request.window == OS_INVALID_HANDLE || request.address == 0U) {
        return false;
    }
    g_window.identifier = request.identifier;
    g_window.width = request.width;
    g_window.height = request.height;
    g_window.pixels = (uint32_t *)(uintptr_t)request.address;
    return true;
}

static bool key_matches(const os_input_event_t *input, uint32_t hid,
                        uint32_t ascii) {
    return input != 0 && (input->code == hid || input->code == ascii);
}

static bool handle_key(const os_window_event_t *event) {
    const os_input_event_t *input = event != 0 ? &event->input : 0;

    if (event == 0 || event->type != OS_WINDOW_EVENT_INPUT || input == 0 ||
        input->type != OS_INPUT_EVENT_KEY) return false;
    if (input->code == 0xE1U || input->code == 0xE5U) {
        g_shift = input->value != OS_INPUT_VALUE_RELEASE;
        return false;
    }
    if (input->code == 0xE0U || input->code == 0xE4U) {
        g_ctrl = input->value != OS_INPUT_VALUE_RELEASE;
        return false;
    }
    if (input->value == OS_INPUT_VALUE_RELEASE) return false;
    if ((g_ctrl && key_matches(input, 0x14U, 'Q')) ||
        key_matches(input, 0x29U, 0x1BU)) {
        g_running = false;
        return true;
    }
    if (input->code == 0x4FU) {
        g_pan_x -= 32;
    } else if (input->code == 0x50U) {
        g_pan_x += 32;
    } else if (input->code == 0x52U) {
        g_pan_y -= 32;
    } else if (input->code == 0x51U) {
        g_pan_y += 32;
    } else if (g_ctrl && input->code == 0x2EU) {
        (void)liteos_text_adjust(1);
    } else if (g_ctrl && input->code == 0x2DU) {
        (void)liteos_text_adjust(-1);
    } else if ((input->code == 0x2EU && g_shift) || input->code == '+') {
        change_zoom(25);
    } else if ((input->code == 0x2DU && !g_shift) || input->code == '-') {
        change_zoom(-25);
    } else if (key_matches(input, 0x27U, '0')) {
        reset_view();
    } else {
        return false;
    }
    return true;
}

static bool handle_event(const os_window_event_t *event) {
    const os_input_event_t *input;

    if (event == 0) return false;
    if (event->type == OS_WINDOW_EVENT_CLOSE_REQUEST) {
        g_running = false;
        return true;
    }
    if (event->type == OS_WINDOW_EVENT_RESIZE) {
        if (event->resize.width == 0U || event->resize.height == 0U ||
            (uint64_t)event->resize.width * event->resize.height *
                sizeof(uint32_t) > event->resize.buffer_size) return false;
        g_window.width = event->resize.width;
        g_window.height = event->resize.height;
        reset_view();
        return true;
    }
    if (event->type != OS_WINDOW_EVENT_INPUT) return false;
    input = &event->input;
    if (input->type == OS_INPUT_EVENT_BUTTON &&
        input->code == OS_INPUT_BUTTON_LEFT &&
        input->value == OS_INPUT_VALUE_PRESS &&
        user_client_chrome_close_hit(event->pointer_x, event->pointer_y,
                                     g_window.width,
                                     USER_CLIENT_CHROME_HEIGHT)) {
        g_running = false;
        return true;
    }
    if (input->type == OS_INPUT_EVENT_RELATIVE &&
        input->code == OS_INPUT_REL_WHEEL && input->value != 0) {
        if (g_ctrl) {
            (void)liteos_text_adjust(input->value > 0 ? 1 : -1);
            return true;
        }
        change_zoom(input->value > 0 ? 25 : -25);
        return true;
    }
    return handle_key(event);
}

int main(int argc, char **argv) {
    copy_path(IMAGEVIEW_DEFAULT_PATH);
    if (argc > 1 && argv != 0 && argv[1] != 0 && argv[1][0] != '\0') {
        copy_path(argv[1]);
    }
    if (!liteos_text_init(LITEOS_TEXT_DEFAULT_SIZE)) {
        printf("IMAGEVIEW_FAIL stage=font\n");
        return 1;
    }
    if (!load_image_file()) {
        printf("IMAGEVIEW_FAIL stage=decode path=%s\n", g_path);
        imageview_cleanup();
        return 1;
    }
    if (!create_window()) {
        printf("IMAGEVIEW_FAIL stage=window\n");
        imageview_cleanup();
        return 1;
    }
    reset_view();
    render();
    while (g_running) {
        os_window_event_read_t request = {0};
        request.hdr.size = sizeof(request);
        request.hdr.version = OS_SYSCALL_ABI_VERSION;
        request.identifier = g_window.identifier;
        request.timeout_ns = IMAGEVIEW_EVENT_WAIT;
        int64_t status = liteos_syscall6(
            OS_SYS_WINDOW_EVENT_READ, (uint64_t)(uintptr_t)&request,
            0U, 0U, 0U, 0U, 0U);
        if (status == 0) {
            if (handle_event(&request.event) && g_running) render();
        } else if (status != -11 && status != -110) {
            __asm__ volatile("pause");
        }
    }
    imageview_cleanup();
    return 0;
}
