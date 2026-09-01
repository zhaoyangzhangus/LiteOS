#include <stdint.h>
#include <stdbool.h>

#include <uapi/all.h>

#include "../../runtime/liteos_text.h"
#include "../../client_chrome.h"

#define NETMGR_MAP_BASE          0x0C000000ULL
#define NETMGR_EVENT_TIMEOUT     100000000ULL
#define NETMGR_REFRESH_TICKS     5U
#define NETMGR_MIN_WIDTH         520U
#define NETMGR_MIN_HEIGHT        320U
#define NETMGR_CONTENT_BACKGROUND 0x00F7F8FAU
#define NETMGR_CARD_BACKGROUND    0x00FFFFFFU
#define NETMGR_CARD_BORDER        0x00E1E5E9U
#define NETMGR_SECTION_TEXT       0x005B6C7AU
#define NETMGR_VALUE_TEXT         0x003D4852U
#define NETMGR_HINT_TEXT          0x006B7782U
#define NETMGR_COMMAND_TEXT       0x005B6670U
#define NETMGR_CONNECTED_TEXT     0x002D8A63U
#define NETMGR_DISCONNECTED_TEXT  0x00B56A28U
#define NETMGR_FOOTER_BACKGROUND  0x00E9EDF1U
#define NETMGR_ERROR_TEXT         0x00B24F3EU

typedef struct netmgr_window {
    os_handle_t handle;
    uint32_t identifier;
    uint32_t width;
    uint32_t height;
    uint32_t *pixels;
} netmgr_window_t;

static netmgr_window_t g_window = {
    OS_INVALID_HANDLE, 0U, 0U, 0U, 0
};
static os_net_status_t g_status;
static bool g_status_valid;
static bool g_ctrl;
static uint32_t g_refresh_tick;

#define NETMGR_DAMAGE_CAPACITY 4U
typedef struct netmgr_damage_rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} netmgr_damage_rect_t;
static netmgr_damage_rect_t g_damage[NETMGR_DAMAGE_CAPACITY];
static uint32_t g_damage_count;
static bool g_damage_full;

static void netmgr_damage_all(void) {
    g_damage_count = 0U;
    g_damage_full = true;
}

static void netmgr_damage_reset(void) {
    g_damage_count = 0U;
    g_damage_full = false;
}

static void netmgr_damage_rect(uint32_t x, uint32_t y,
                               uint32_t width, uint32_t height) {
    uint32_t right;
    uint32_t bottom;
    if (g_damage_full || width == 0U || height == 0U) return;
    if (x >= g_window.width || y >= g_window.height) return;
    if (width > g_window.width - x) width = g_window.width - x;
    if (height > g_window.height - y) height = g_window.height - y;
    right = x + width;
    bottom = y + height;
    for (uint32_t index = 0U; index < g_damage_count; ++index) {
        netmgr_damage_rect_t *current = &g_damage[index];
        uint32_t current_right = current->x + current->width;
        uint32_t current_bottom = current->y + current->height;
        if (right < current->x || current_right < x ||
            bottom < current->y || current_bottom < y) continue;
        if (x > current->x) x = current->x;
        if (y > current->y) y = current->y;
        if (right < current_right) right = current_right;
        if (bottom < current_bottom) bottom = current_bottom;
        current->x = x;
        current->y = y;
        current->width = right - x;
        current->height = bottom - y;
        return;
    }
    if (g_damage_count >= NETMGR_DAMAGE_CAPACITY) {
        netmgr_damage_all();
        return;
    }
    g_damage[g_damage_count++] = (netmgr_damage_rect_t){x, y, width, height};
}

void __main(void) {
}

static int64_t netmgr_syscall_one(uint64_t number, uint64_t arg0) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = arg0;
    __asm__ volatile ("syscall" : "+a"(rax), "+D"(rdi) :
                      : "rcx", "r11", "memory");
    return (int64_t)rax;
}

__attribute__((noreturn)) static void netmgr_exit(uint64_t status) {
    if (g_window.handle != OS_INVALID_HANDLE) {
        (void)netmgr_syscall_one(OS_SYS_HANDLE_CLOSE, g_window.handle);
        g_window.handle = OS_INVALID_HANDLE;
    }
    (void)netmgr_syscall_one(OS_SYS_THREAD_EXIT, status);
    for (;;) __asm__ volatile ("pause");
}

static int64_t netmgr_get_status(os_net_status_t *status) {
    if (status == 0) return -22;
    *status = (os_net_status_t){0};
    status->hdr.size = sizeof(*status);
    status->hdr.version = OS_SYSCALL_ABI_VERSION;
    return netmgr_syscall_one(OS_SYS_NET_GET_STATUS, (uint64_t)status);
}

static uint32_t text_length(const char *text) {
    uint32_t length = 0U;
    if (text == 0) return 0U;
    while (text[length] != '\0') ++length;
    return length;
}

static void append_character(char *destination, uint32_t capacity, char value) {
    uint32_t length;
    if (destination == 0 || capacity == 0U) return;
    length = text_length(destination);
    if (length + 1U >= capacity) return;
    destination[length] = value;
    destination[length + 1U] = '\0';
}

static void append_text(char *destination, uint32_t capacity, const char *source) {
    uint32_t length;
    uint32_t index = 0U;
    if (destination == 0 || source == 0 || capacity == 0U) return;
    length = text_length(destination);
    while (length + 1U < capacity && source[index] != '\0') {
        destination[length++] = source[index++];
    }
    destination[length] = '\0';
}

static void append_decimal(char *destination, uint32_t capacity, uint64_t value) {
    char digits[24];
    uint32_t count = 0U;
    do {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0U && count < sizeof(digits));
    while (count != 0U) {
        append_character(destination, capacity, digits[--count]);
    }
}

static char hex_digit(uint8_t value) {
    value &= 0x0FU;
    return value < 10U ? (char)('0' + value) :
                         (char)('A' + value - 10U);
}

static void append_hex_byte(char *destination, uint32_t capacity, uint8_t value) {
    append_character(destination, capacity, hex_digit((uint8_t)(value >> 4U)));
    append_character(destination, capacity, hex_digit(value));
}

static void append_hex_group(char *destination, uint32_t capacity,
                             uint16_t value) {
    char digits[4];
    uint32_t first = 0U;
    for (uint32_t index = 0U; index < 4U; ++index) {
        digits[3U - index] = hex_digit((uint8_t)value);
        value >>= 4U;
    }
    while (first < 3U && digits[first] == '0') ++first;
    for (uint32_t index = first; index < 4U; ++index) {
        append_character(destination, capacity, digits[index]);
    }
}

static void format_ipv4(char *output, uint32_t capacity, uint32_t address) {
    if (output == 0 || capacity == 0U) return;
    output[0] = '\0';
    if (address == 0U) {
        append_text(output, capacity, "Not configured");
        return;
    }
    append_decimal(output, capacity, (address >> 24U) & 0xFFU);
    append_character(output, capacity, '.');
    append_decimal(output, capacity, (address >> 16U) & 0xFFU);
    append_character(output, capacity, '.');
    append_decimal(output, capacity, (address >> 8U) & 0xFFU);
    append_character(output, capacity, '.');
    append_decimal(output, capacity, address & 0xFFU);
}

static void format_ipv4_prefix(char *output, uint32_t capacity,
                               uint32_t address, uint8_t prefix) {
    format_ipv4(output, capacity, address);
    if (address != 0U) {
        append_character(output, capacity, '/');
        append_decimal(output, capacity, prefix);
    }
}

static void format_mac(char *output, uint32_t capacity, const uint8_t mac[6]) {
    if (output == 0 || capacity == 0U || mac == 0) return;
    output[0] = '\0';
    for (uint32_t index = 0U; index < 6U; ++index) {
        if (index != 0U) append_character(output, capacity, ':');
        append_hex_byte(output, capacity, mac[index]);
    }
}

static void format_ipv6(char *output, uint32_t capacity,
                        const uint8_t address[16], bool configured) {
    uint16_t groups[8];
    uint32_t best_start = 8U;
    uint32_t best_length = 0U;
    if (output == 0 || capacity == 0U) return;
    output[0] = '\0';
    if (!configured) {
        append_text(output, capacity, "Not configured");
        return;
    }
    for (uint32_t group = 0U; group < 8U; ++group) {
        groups[group] = (uint16_t)(((uint16_t)address[group * 2U] << 8U) |
                                   address[group * 2U + 1U]);
    }
    for (uint32_t start = 0U; start < 8U;) {
        uint32_t length = 0U;
        while (start + length < 8U && groups[start + length] == 0U) ++length;
        if (length >= 2U && length > best_length) {
            best_start = start;
            best_length = length;
        }
        start += length != 0U ? length : 1U;
    }
    for (uint32_t group = 0U; group < 8U;) {
        if (group == best_start) {
            append_character(output, capacity, ':');
            append_character(output, capacity, ':');
            group += best_length;
            continue;
        }
        if (text_length(output) != 0U &&
            output[text_length(output) - 1U] != ':') {
            append_character(output, capacity, ':');
        }
        append_hex_group(output, capacity, groups[group]);
        ++group;
    }
}

static void fill_rect(uint32_t x, uint32_t y,
                      uint32_t width, uint32_t height, uint32_t color) {
    if (g_window.pixels == 0 || x >= g_window.width || y >= g_window.height) {
        return;
    }
    if (width > g_window.width - x) width = g_window.width - x;
    if (height > g_window.height - y) height = g_window.height - y;
    for (uint32_t row = 0U; row < height; ++row) {
        for (uint32_t column = 0U; column < width; ++column) {
            g_window.pixels[
                (uint64_t)(y + row) * g_window.width + x + column] = color;
        }
    }
}

static void draw_text(uint32_t x, uint32_t y,
                      const char *text, uint32_t color) {
    liteos_text_draw(g_window.pixels, g_window.width,
                     g_window.width, g_window.height,
                     (int32_t)x, (int32_t)y, text, color);
}

static void draw_text_clipped(uint32_t x, uint32_t y, uint32_t right,
                              const char *text, uint32_t color) {
    uint32_t max_right;
    if (text == 0 || x >= g_window.width || y >= g_window.height) return;
    max_right = right < g_window.width ? right : g_window.width;
    if (max_right <= x) return;
    liteos_text_draw_clipped(g_window.pixels, g_window.width,
                             g_window.width, g_window.height,
                             (int32_t)x, (int32_t)y, max_right,
                             text, color);
}

static void draw_card(uint32_t x, uint32_t y,
                      uint32_t width, uint32_t height) {
    user_client_chrome_round_rect(g_window.pixels, g_window.width,
                                  g_window.width, g_window.height,
                                  x, y, width, height,
                                  NETMGR_CARD_BORDER);
    if (width > 2U && height > 2U) {
        user_client_chrome_round_rect(g_window.pixels, g_window.width,
                                      g_window.width, g_window.height,
                                      x + 1U, y + 1U,
                                      width - 2U, height - 2U,
                                      NETMGR_CARD_BACKGROUND);
    }
}

static void update_window(void) {
    os_window_update_t request = {0};
    if (!g_damage_full && g_damage_count == 0U) return;
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.identifier = g_window.identifier;
    if (g_damage_full) {
        request.width = g_window.width;
        request.height = g_window.height;
        (void)netmgr_syscall_one(OS_SYS_WINDOW_UPDATE, (uint64_t)&request);
    } else {
        for (uint32_t index = 0U; index < g_damage_count; ++index) {
            request.x = (int32_t)g_damage[index].x;
            request.y = (int32_t)g_damage[index].y;
            request.width = g_damage[index].width;
            request.height = g_damage[index].height;
            (void)netmgr_syscall_one(OS_SYS_WINDOW_UPDATE,
                                     (uint64_t)&request);
        }
    }
    netmgr_damage_reset();
}

static void render(void) {
    char value[96];
    char line[128];
    uint32_t content_width;
    uint32_t left_width;
    uint32_t right_x;
    uint32_t right_width;
    uint32_t left_right;
    uint32_t right_right;
    bool hardware;
    bool link_up;
    bool ipv6;

    if (g_window.pixels == 0 || g_window.width == 0U ||
        g_window.height == 0U) return;
    if (!g_damage_full && g_damage_count == 0U) netmgr_damage_all();

    hardware = g_status_valid &&
        (g_status.flags & OS_NET_STATUS_HARDWARE_PRESENT) != 0U;
    link_up = g_status_valid &&
        (g_status.flags & OS_NET_STATUS_LINK_UP) != 0U;
    ipv6 = g_status_valid &&
        (g_status.flags & OS_NET_STATUS_IPV6_CONFIGURED) != 0U;

    fill_rect(0U, 0U, g_window.width, g_window.height,
              NETMGR_CONTENT_BACKGROUND);

    fill_rect(0U, 0U, g_window.width, USER_CLIENT_CHROME_HEIGHT,
              USER_CLIENT_CHROME_BACKGROUND);
    fill_rect(0U, USER_CLIENT_CHROME_HEIGHT - 1U, g_window.width, 1U,
              USER_CLIENT_CHROME_SEPARATOR);
    user_client_chrome_app_icon(g_window.pixels, g_window.width,
                                g_window.width, g_window.height,
                                16U, USER_CLIENT_CHROME_TITLE_Y,
                                USER_CLIENT_CHROME_ICON_NETWORK);
    draw_text(50U, USER_CLIENT_CHROME_TITLE_Y, "NETWORK",
              USER_CLIENT_CHROME_TEXT);

    content_width = g_window.width > 36U ? g_window.width - 36U :
                                           g_window.width;

    if (content_width >= 640U) {
        left_width = (content_width - 16U) / 2U;
        right_x = 18U + left_width + 16U;
        right_width = content_width - left_width - 16U;
    } else {
        left_width = content_width;
        right_x = 18U;
        right_width = content_width;
    }
    left_right = 18U + left_width;
    right_right = right_x + right_width;

    draw_card(18U, 68U, left_width, 86U);
    draw_text_clipped(34U, 82U, left_right - 10U,
                      "CONNECTION", NETMGR_SECTION_TEXT);
    draw_text_clipped(34U, 110U, left_right - 10U,
                      !g_status_valid ? "Status unavailable" :
                      !hardware ? "No network hardware" :
                      link_up ? "Connected" : "Disconnected",
                      link_up ? NETMGR_CONNECTED_TEXT : NETMGR_DISCONNECTED_TEXT);
    draw_text_clipped(34U, 132U, left_right - 10U,
                      "DHCP service: netd", NETMGR_HINT_TEXT);

    draw_card(18U, 166U, left_width, 138U);
    draw_text_clipped(34U, 180U, left_right - 10U,
                      "IP CONFIGURATION", NETMGR_SECTION_TEXT);

    line[0] = '\0';
    append_text(line, sizeof(line), "IPv4   ");
    if (g_status_valid) {
        format_ipv4_prefix(value, sizeof(value),
                           g_status.ipv4_address,
                           g_status.ipv4_prefix_length);
    } else {
        value[0] = '\0';
        append_text(value, sizeof(value), "Unavailable");
    }
    append_text(line, sizeof(line), value);
    draw_text_clipped(34U, 208U, left_right - 10U,
                      line, NETMGR_VALUE_TEXT);

    line[0] = '\0';
    append_text(line, sizeof(line), "Gateway ");
    if (g_status_valid) {
        format_ipv4(value, sizeof(value), g_status.ipv4_gateway);
    } else {
        value[0] = '\0';
        append_text(value, sizeof(value), "Unavailable");
    }
    append_text(line, sizeof(line), value);
    draw_text_clipped(34U, 234U, left_right - 10U,
                      line, NETMGR_VALUE_TEXT);

    line[0] = '\0';
    append_text(line, sizeof(line), "IPv6   ");
    if (g_status_valid) {
        format_ipv6(value, sizeof(value), g_status.ipv6_address, ipv6);
    } else {
        value[0] = '\0';
        append_text(value, sizeof(value), "Unavailable");
    }
    append_text(line, sizeof(line), value);
    draw_text_clipped(34U, 260U, left_right - 10U,
                      line, NETMGR_VALUE_TEXT);

    if (content_width >= 640U) {
        draw_card(right_x, 68U, right_width, 236U);
        draw_text_clipped(right_x + 16U, 82U, right_right - 10U,
                          "ADAPTER", NETMGR_SECTION_TEXT);

        line[0] = '\0';
        append_text(line, sizeof(line), "MAC ");
        if (g_status_valid) {
            format_mac(value, sizeof(value), g_status.mac);
        } else {
            value[0] = '\0';
            append_text(value, sizeof(value), "--:--:--:--:--:--");
        }
        append_text(line, sizeof(line), value);
        draw_text_clipped(right_x + 16U, 112U, right_right - 10U,
                          line, NETMGR_VALUE_TEXT);

        line[0] = '\0';
        append_text(line, sizeof(line), "Link changes ");
        append_decimal(line, sizeof(line),
                       g_status_valid ? g_status.link_transitions : 0U);
        draw_text_clipped(right_x + 16U, 142U, right_right - 10U,
                          line, NETMGR_VALUE_TEXT);

        line[0] = '\0';
        append_text(line, sizeof(line), "Reset count  ");
        append_decimal(line, sizeof(line),
                       g_status_valid ? g_status.reset_count : 0U);
        draw_text_clipped(right_x + 16U, 172U, right_right - 10U,
                          line, NETMGR_VALUE_TEXT);

        draw_text_clipped(right_x + 16U, 218U, right_right - 10U,
                          "R        Refresh now", NETMGR_COMMAND_TEXT);
        draw_text_clipped(right_x + 16U, 244U, right_right - 10U,
                          "Ctrl+/- Font   Ctrl+Q Close", NETMGR_COMMAND_TEXT);
        draw_text_clipped(right_x + 16U, 270U, right_right - 10U,
                          "Auto refresh: 500 ms", NETMGR_HINT_TEXT);
    } else if (g_window.height >= 390U) {
        draw_card(18U, 316U, left_width, 62U);
        draw_text_clipped(34U, 330U, left_right - 10U,
                          "R Refresh    Ctrl+/- Font    Ctrl+Q Close",
                          NETMGR_COMMAND_TEXT);
    }

    if (g_window.height > 42U) {
        uint32_t footer_y = g_window.height - 34U;
        fill_rect(0U, footer_y, g_window.width, 34U,
                  NETMGR_CONTENT_BACKGROUND);
        user_client_chrome_round_rect(g_window.pixels, g_window.width,
                                      g_window.width, g_window.height,
                                      12U, footer_y + 3U,
                                      g_window.width > 24U ?
                                      g_window.width - 24U : g_window.width,
                                      28U, NETMGR_FOOTER_BACKGROUND);
        draw_text_clipped(24U, g_window.height - 29U,
                          g_window.width > 24U ? g_window.width - 24U :
                          g_window.width,
                          g_status_valid ?
                          "Live kernel network status" :
                          "Network status read failed",
                          g_status_valid ? NETMGR_COMMAND_TEXT : NETMGR_ERROR_TEXT);
    }

    user_client_chrome_close(g_window.pixels, g_window.width,
                             g_window.width, g_window.height,
                             USER_CLIENT_CHROME_HEIGHT,
                             USER_CLIENT_CHROME_CLOSE_BG,
                             USER_CLIENT_CHROME_CLOSE_FG);
    user_client_chrome_frame(g_window.pixels, g_window.width,
                             g_window.width, g_window.height,
                             USER_CLIENT_CHROME_HEIGHT);

    update_window();
}

static bool refresh_status(void) {
    os_net_status_t status;
    if (netmgr_get_status(&status) < 0) {
        g_status_valid = false;
        netmgr_damage_rect(0U, 60U, g_window.width,
                           g_window.height > 60U ? g_window.height - 60U : 0U);
        render();
        return false;
    }
    g_status = status;
    g_status_valid = true;
    netmgr_damage_rect(0U, 60U, g_window.width,
                       g_window.height > 60U ? g_window.height - 60U : 0U);
    render();
    return true;
}

static bool create_window(void) {
    os_display_info_t display = {0};
    os_window_create_t request = {0};

    display.hdr.size = sizeof(display);
    display.hdr.version = OS_SYSCALL_ABI_VERSION;
    if (netmgr_syscall_one(OS_SYS_DISPLAY_GET_INFO,
                           (uint64_t)&display) < 0 ||
        display.width < 320U || display.height < 240U) {
        return false;
    }

    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.width = display.width > 760U ? 720U :
                    (display.width > 48U ? display.width - 48U :
                                           display.width);
    request.height = display.height > 500U ? 430U :
                     (display.height > 70U ? display.height - 70U :
                                             display.height);
    if (request.width < NETMGR_MIN_WIDTH &&
        display.width >= NETMGR_MIN_WIDTH) {
        request.width = NETMGR_MIN_WIDTH;
    }
    if (request.height < NETMGR_MIN_HEIGHT &&
        display.height >= NETMGR_MIN_HEIGHT) {
        request.height = NETMGR_MIN_HEIGHT;
    }
    request.x = display.width > request.width ?
        (int32_t)((display.width - request.width) / 2U) : 0;
    request.y = display.height > request.height ?
        (int32_t)((display.height - request.height) / 2U) : 0;
    request.flags = OS_WINDOW_VISIBLE |
                    OS_WINDOW_RESIZABLE |
                    OS_WINDOW_CLIENT_DECORATIONS;
    request.background = NETMGR_CONTENT_BACKGROUND;
    request.title[0] = 'N';
    request.title[1] = 'E';
    request.title[2] = 'T';
    request.title[3] = 'W';
    request.title[4] = 'O';
    request.title[5] = 'R';
    request.title[6] = 'K';
    request.title[7] = '\0';
    request.address = NETMGR_MAP_BASE;

    if (netmgr_syscall_one(OS_SYS_WINDOW_CREATE,
                           (uint64_t)&request) != 0 ||
        request.window == OS_INVALID_HANDLE ||
        request.identifier == 0U ||
        request.address == 0U) {
        return false;
    }

    g_window.handle = request.window;
    g_window.identifier = request.identifier;
    g_window.width = request.width;
    g_window.height = request.height;
    g_window.pixels = (uint32_t *)(uintptr_t)request.address;
    return true;
}

static void handle_key(const os_window_event_t *event) {
    const os_input_event_t *input;

    if (event == 0 || event->type != OS_WINDOW_EVENT_INPUT) return;
    input = &event->input;
    if (input->type != OS_INPUT_EVENT_KEY) return;

    if (input->code == 0xE0U || input->code == 0xE4U) {
        g_ctrl = input->value != OS_INPUT_VALUE_RELEASE;
        return;
    }
    if (input->value == OS_INPUT_VALUE_RELEASE) return;

    if (g_ctrl &&
        (input->code == 0x14U || input->code == (uint32_t)'Q')) {
        netmgr_exit(0U);
    }
    if (g_ctrl && input->code == 0x2EU) {
        (void)liteos_text_adjust(1);
        netmgr_damage_all();
        render();
        return;
    }
    if (g_ctrl && input->code == 0x2DU) {
        (void)liteos_text_adjust(-1);
        netmgr_damage_all();
        render();
        return;
    }
    if (input->code == 0x15U || input->code == (uint32_t)'R') {
        g_refresh_tick = 0U;
        (void)refresh_status();
    }
}

static void handle_event(const os_window_event_t *event) {
    uint64_t pixels;

    if (event == 0) return;

    /* V2.3 cooperative close */
    if (event->type ==
        OS_WINDOW_EVENT_CLOSE_REQUEST) {

        netmgr_exit(0U);
    }

    if (event->type == OS_WINDOW_EVENT_RESIZE) {
        if (event->resize.width == 0U ||
            event->resize.height == 0U) return;
        pixels = (uint64_t)event->resize.width *
                 event->resize.height;
        if (pixels >
            event->resize.buffer_size / sizeof(uint32_t)) return;
        g_window.width = event->resize.width;
        g_window.height = event->resize.height;
        netmgr_damage_all();
        render();
        return;
    }
    if (event->type == OS_WINDOW_EVENT_INPUT &&
        event->input.type == OS_INPUT_EVENT_RELATIVE &&
        event->input.code == OS_INPUT_REL_WHEEL &&
        event->input.value != 0 && g_ctrl) {
        (void)liteos_text_adjust(event->input.value > 0 ? 1 : -1);
        netmgr_damage_all();
        render();
        return;
    }
    if (event->type == OS_WINDOW_EVENT_INPUT &&
        event->input.type == OS_INPUT_EVENT_BUTTON &&
        event->input.code == OS_INPUT_BUTTON_LEFT &&
        event->input.value == OS_INPUT_VALUE_PRESS &&
        user_client_chrome_close_hit(event->pointer_x, event->pointer_y,
                                     g_window.width,
                                     USER_CLIENT_CHROME_HEIGHT)) {
        netmgr_exit(0U);
    }
    handle_key(event);
}

static int netmgr_main(void) {
    if (!liteos_text_init(LITEOS_TEXT_DEFAULT_SIZE) ||
        !create_window()) return 1;
    /* The first status refresh only dirties the content area.  Paint and
     * publish the complete client chrome once so the close button is visible
     * immediately when the window opens. */
    netmgr_damage_all();
    (void)refresh_status();

    for (;;) {
        os_window_event_read_t request = {0};
        int64_t status;

        request.hdr.size = sizeof(request);
        request.hdr.version = OS_SYSCALL_ABI_VERSION;
        request.identifier = g_window.identifier;
        request.timeout_ns = NETMGR_EVENT_TIMEOUT;

        status = netmgr_syscall_one(OS_SYS_WINDOW_EVENT_READ,
                                    (uint64_t)&request);
        if (status == 0) {
            handle_event(&request.event);
        } else if (status != -11 && status != -110) {
            __asm__ volatile ("pause");
        }

        if (++g_refresh_tick >= NETMGR_REFRESH_TICKS) {
            g_refresh_tick = 0U;
            (void)refresh_status();
        }
    }
}

int main(void) {
    return netmgr_main();
}

__attribute__((noreturn)) void netmgr_entry(void) {
    int status = netmgr_main();
    netmgr_exit(status == 0 ? 0U : 1U);
}
