#include <stdint.h>
#include <stdbool.h>

#include <uapi/all.h>

#include "../font12x24.h"
#include "../client_chrome.h"

#define NOTEPAD_TEXT_CAPACITY 8192U
#define NOTEPAD_MAP_BASE      0x08000000ULL
#define NOTEPAD_EVENT_TIMEOUT 100000000ULL
#define NOTEPAD_FILE_PATH     "/notes.txt"
#define NOTEPAD_UNDO_DEPTH    8U
#define NOTEPAD_PATH_CAPACITY 256U

typedef struct notepad_window {
    os_handle_t handle;
    uint32_t identifier;
    uint32_t width;
    uint32_t height;
    uint32_t *pixels;
} notepad_window_t;

typedef struct notepad_snapshot {
    char text[NOTEPAD_TEXT_CAPACITY];
    size_t length;
    size_t cursor;
} notepad_snapshot_t;

static const uint8_t g_upper_font[37][7] = {
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

static const uint8_t g_lower_font[26][7] = {
    {0,0,14,1,15,17,15}, {16,16,30,17,17,30,0}, {0,0,14,16,16,14,0},
    {1,1,15,17,17,15,0}, {0,0,14,17,31,16,14}, {6,9,28,8,8,8,0},
    {0,15,17,15,1,14,0}, {16,16,30,17,17,17,0}, {4,0,12,4,4,14,0},
    {2,0,6,2,18,12,0}, {16,16,18,20,24,20,0}, {12,4,4,4,4,14,0},
    {0,0,27,21,21,17,0}, {0,0,30,17,17,17,0}, {0,0,14,17,17,14,0},
    {0,0,30,17,30,16,16}, {0,0,15,17,15,1,1}, {0,0,22,25,16,16,0},
    {0,0,15,16,14,1,30}, {8,8,30,8,9,6,0}, {0,0,17,17,17,15,0},
    {0,0,17,17,10,4,0}, {0,0,17,21,21,10,0}, {0,0,17,10,4,10,17},
    {0,0,17,17,15,1,14}, {0,0,31,2,4,8,31},
};

static const uint8_t g_symbol_font[32][7] = {
    {4,4,4,4,4,0,4}, {10,10,0,0,0,0,0}, {10,31,10,10,31,10,0},
    {4,15,20,14,5,30,4}, {25,2,4,8,19,0,0}, {12,18,20,8,21,18,13},
    {4,4,0,0,0,0,0}, {2,4,8,8,8,4,2}, {8,4,2,2,2,4,8},
    {0,10,4,31,4,10,0}, {0,4,4,31,4,4,0}, {0,0,0,0,4,4,8},
    {0,0,0,31,0,0,0}, {0,0,0,0,0,4,0}, {1,2,4,8,16,0,0},
    {0,4,0,0,4,0,0}, {0,4,0,0,4,4,8}, {2,4,8,16,8,4,2},
    {0,0,31,0,31,0,0}, {8,4,2,1,2,4,8}, {14,17,1,2,4,0,4},
    {14,17,29,21,29,16,14}, {14,8,8,8,8,8,14}, {16,8,4,2,1,0,0},
    {14,2,2,2,2,2,14}, {4,10,17,0,0,0,0}, {0,0,0,0,0,0,31},
    {4,2,0,0,0,0,0}, {2,4,8,4,2,4,8}, {4,4,4,4,4,4,4},
    {8,4,2,4,8,4,2}, {0,0,9,18,0,0,0},
};

static const char g_symbol_chars[] = "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~";
static char g_file_path[NOTEPAD_PATH_CAPACITY] = NOTEPAD_FILE_PATH;
static notepad_window_t g_window = {OS_INVALID_HANDLE, 0U, 0U, 0U, 0};
static char g_text[NOTEPAD_TEXT_CAPACITY];
static size_t g_text_length;
static size_t g_cursor;
static uint32_t g_scroll_line;
static bool g_follow_cursor = true;
static bool g_shift;
static bool g_ctrl;
static bool g_dirty;
static char g_status[128] = "CTRL+S SAVE  CTRL+Z UNDO  CTRL+Q EXIT";
static notepad_snapshot_t g_undo[NOTEPAD_UNDO_DEPTH];
static notepad_snapshot_t g_redo[NOTEPAD_UNDO_DEPTH];
static uint32_t g_undo_count;
static uint32_t g_redo_count;
static char g_saved_text[NOTEPAD_TEXT_CAPACITY];
static size_t g_saved_length;
static uint32_t *g_target;
static uint32_t g_target_width;
static uint32_t g_target_height;

#define NOTEPAD_DAMAGE_CAPACITY 8U
typedef struct notepad_damage_rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} notepad_damage_rect_t;
static notepad_damage_rect_t g_damage[NOTEPAD_DAMAGE_CAPACITY];
static uint32_t g_damage_count;
static bool g_damage_full;

static void notepad_damage_all(void) {
    g_damage_count = 0U;
    g_damage_full = true;
}

static void notepad_damage_reset(void) {
    g_damage_count = 0U;
    g_damage_full = false;
}

static void notepad_damage_rect(uint32_t x, uint32_t y,
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
        notepad_damage_rect_t *current = &g_damage[index];
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
    if (g_damage_count >= NOTEPAD_DAMAGE_CAPACITY) {
        notepad_damage_all();
        return;
    }
    g_damage[g_damage_count++] = (notepad_damage_rect_t){x, y,
                                                         width, height};
}

static void notepad_damage_editor(void) {
    uint32_t top = USER_CLIENT_CHROME_HEIGHT;
    uint32_t bottom = g_window.height > 32U ? g_window.height - 32U : top;
    if (bottom > top) notepad_damage_rect(0U, top, g_window.width,
                                          bottom - top);
}

static void notepad_damage_title(void) {
    notepad_damage_rect(0U, 0U, g_window.width,
                        USER_CLIENT_CHROME_HEIGHT);
}

static void notepad_damage_status(void) {
    uint32_t y = g_window.height > 32U ? g_window.height - 32U : 0U;
    notepad_damage_rect(0U, y, g_window.width, g_window.height - y);
}

/* MinGW 在标准 main 入口前会发出 __main；LiteOS 用户程序不链接 CRT。 */
void __main(void) {
}

static int64_t notepad_syscall_one(uint64_t number, uint64_t arg0) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = arg0;
    __asm__ volatile ("syscall" : "+a"(rax), "+D"(rdi) :
                      : "rcx", "r11", "memory");
    return (int64_t)rax;
}

static int64_t notepad_syscall_four(uint64_t number, uint64_t arg0,
                                    uint64_t arg1, uint64_t arg2,
                                    uint64_t arg3) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = arg0;
    register uint64_t rsi __asm__("rsi") = arg1;
    register uint64_t rdx __asm__("rdx") = arg2;
    register uint64_t r10 __asm__("r10") = arg3;
    __asm__ volatile ("syscall" : "+a"(rax), "+D"(rdi), "+S"(rsi),
                      "+d"(rdx), "+r"(r10) : : "rcx", "r11", "memory");
    return (int64_t)rax;
}

__attribute__((noreturn)) static void notepad_exit(uint64_t status) {
    (void)notepad_syscall_one(OS_SYS_THREAD_EXIT, status);
    for (;;) __asm__ volatile ("pause");
}

static void copy_text(char *destination, uint32_t capacity, const char *source) {
    uint32_t index = 0U;
    if (destination == 0 || source == 0 || capacity == 0U) return;
    while (index + 1U < capacity && source[index] != '\0') {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = '\0';
}

static bool set_file_path(const char *argument) {
    uint32_t length = 0U;
    uint32_t prefix_length = 0U;
    if (argument == 0 || argument[0] == '\0') return false;
    if (argument[0] == '/') {
        while (argument[length] != '\0') {
            if (++length >= NOTEPAD_PATH_CAPACITY) return false;
        }
        for (uint32_t index = 0U; index <= length; ++index) {
            g_file_path[index] = argument[index];
        }
        return true;
    }
    static const char root_prefix[] = "/";
    while (root_prefix[prefix_length] != '\0') ++prefix_length;
    while (argument[length] != '\0') {
        if (prefix_length + length + 1U >= NOTEPAD_PATH_CAPACITY) return false;
        ++length;
    }
    for (uint32_t index = 0U; index < prefix_length; ++index) {
        g_file_path[index] = root_prefix[index];
    }
    for (uint32_t index = 0U; index <= length; ++index) {
        g_file_path[prefix_length + index] = argument[index];
    }
    return true;
}

static void set_status(const char *text) {
    copy_text(g_status, sizeof(g_status), text);
}

static void snapshot_capture(notepad_snapshot_t *snapshot) {
    if (snapshot == 0) return;
    snapshot->length = g_text_length;
    snapshot->cursor = g_cursor;
    for (size_t index = 0U; index <= g_text_length; ++index) {
        snapshot->text[index] = g_text[index];
    }
}

static void snapshot_restore(const notepad_snapshot_t *snapshot) {
    if (snapshot == 0 || snapshot->length >= NOTEPAD_TEXT_CAPACITY) return;
    g_text_length = snapshot->length;
    g_cursor = snapshot->cursor <= g_text_length ? snapshot->cursor : g_text_length;
    for (size_t index = 0U; index <= g_text_length; ++index) {
        g_text[index] = snapshot->text[index];
    }
}

static void snapshot_copy(notepad_snapshot_t *destination,
                          const notepad_snapshot_t *source) {
    if (destination == 0 || source == 0) return;
    destination->length = source->length;
    destination->cursor = source->cursor;
    for (size_t index = 0U; index < NOTEPAD_TEXT_CAPACITY; ++index) {
        destination->text[index] = source->text[index];
    }
}

static void push_snapshot(notepad_snapshot_t *stack, uint32_t *count) {
    uint32_t limit;
    if (stack == 0 || count == 0) return;
    limit = *count < NOTEPAD_UNDO_DEPTH ? *count : NOTEPAD_UNDO_DEPTH - 1U;
    for (uint32_t index = limit; index != 0U; --index) {
        snapshot_copy(&stack[index], &stack[index - 1U]);
    }
    if (*count < NOTEPAD_UNDO_DEPTH) ++*count;
    snapshot_capture(&stack[0]);
}

static void history_reset(void) {
    g_undo_count = 0U;
    g_redo_count = 0U;
}

static void begin_edit(void) {
    push_snapshot(g_undo, &g_undo_count);
    g_redo_count = 0U;
}

static void update_dirty(void) {
    if (g_text_length != g_saved_length) {
        g_dirty = true;
        return;
    }
    g_dirty = false;
    for (size_t index = 0U; index < g_saved_length; ++index) {
        if (g_text[index] != g_saved_text[index]) {
            g_dirty = true;
            return;
        }
    }
}

static void mark_saved(void) {
    g_saved_length = g_text_length;
    for (size_t index = 0U; index <= g_saved_length; ++index) {
        g_saved_text[index] = g_text[index];
    }
    g_dirty = false;
}

static void undo_document(void) {
    if (g_undo_count == 0U) {
        set_status("NOTHING TO UNDO");
        return;
    }
    push_snapshot(g_redo, &g_redo_count);
    snapshot_restore(&g_undo[0]);
    for (uint32_t index = 1U; index < g_undo_count; ++index) {
        snapshot_copy(&g_undo[index - 1U], &g_undo[index]);
    }
    --g_undo_count;
    update_dirty();
    set_status("UNDO");
}

static void redo_document(void) {
    if (g_redo_count == 0U) {
        set_status("NOTHING TO REDO");
        return;
    }
    push_snapshot(g_undo, &g_undo_count);
    snapshot_restore(&g_redo[0]);
    for (uint32_t index = 1U; index < g_redo_count; ++index) {
        snapshot_copy(&g_redo[index - 1U], &g_redo[index]);
    }
    --g_redo_count;
    update_dirty();
    set_status("REDO");
}

static __attribute__((unused)) const uint8_t *glyph_for(char character) {
    if (character == ' ') return g_upper_font[0];
    if (character >= 'A' && character <= 'Z') return g_upper_font[1U + (uint32_t)(character - 'A')];
    if (character >= 'a' && character <= 'z') return g_lower_font[(uint32_t)(character - 'a')];
    if (character >= '0' && character <= '9') return g_upper_font[27U + (uint32_t)(character - '0')];
    for (uint32_t index = 0U; index + 1U < sizeof(g_symbol_chars); ++index) {
        if (g_symbol_chars[index] == character) return g_symbol_font[index];
    }
    return g_upper_font[0];
}

static void fill_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                      uint32_t color) {
    if (g_target == 0 || x >= g_target_width || y >= g_target_height) return;
    if (width > g_target_width - x) width = g_target_width - x;
    if (height > g_target_height - y) height = g_target_height - y;
    for (uint32_t row = 0U; row < height; ++row) {
        for (uint32_t column = 0U; column < width; ++column) {
            g_target[(uint64_t)(y + row) * g_target_width + x + column] = color;
        }
    }
}

static void draw_text(uint32_t x, uint32_t y, const char *text, uint32_t color) {
    if (text == 0) return;
    for (uint32_t index = 0U; text[index] != '\0'; ++index) {
        font12x24_draw_glyph(g_target, g_target_width,
                             g_target_width, g_target_height,
                             (int32_t)(x + index * FONT12X24_WIDTH),
                             (int32_t)y, text[index], color);
    }
}

static void draw_character(uint32_t x, uint32_t y, char character, uint32_t color) {
    font12x24_draw_glyph(g_target, g_target_width,
                         g_target_width, g_target_height,
                         (int32_t)x, (int32_t)y, character, color);
}

static void advance_position(char character, uint32_t columns,
                             uint32_t *line, uint32_t *column) {
    if (character == '\n') {
        ++*line;
        *column = 0U;
    } else if (character == '\t') {
        uint32_t next = (*column + 4U) & ~3U;
        if (next >= columns) {
            ++*line;
            *column = 0U;
        } else {
            *column = next;
        }
    } else {
        ++*column;
        if (*column >= columns) {
            ++*line;
            *column = 0U;
        }
    }
}

static void cursor_position(uint32_t columns, uint32_t *line, uint32_t *column) {
    *line = 0U;
    *column = 0U;
    for (size_t index = 0U; index < g_cursor; ++index) {
        advance_position(g_text[index], columns, line, column);
    }
}

static size_t offset_at_position(uint32_t columns, uint32_t wanted_line,
                                 uint32_t wanted_column) {
    uint32_t line = 0U;
    uint32_t column = 0U;
    for (size_t index = 0U; index <= g_text_length; ++index) {
        if (line == wanted_line && column >= wanted_column) return index;
        if (index == g_text_length) break;
        if (line > wanted_line) return index - 1U;
        advance_position(g_text[index], columns, &line, &column);
    }
    return g_text_length;
}

static void ensure_cursor_visible(uint32_t columns, uint32_t visible_lines) {
    uint32_t line = 0U;
    uint32_t column = 0U;
    cursor_position(columns, &line, &column);
    (void)column;
    if (line < g_scroll_line) g_scroll_line = line;
    if (visible_lines != 0U && line >= g_scroll_line + visible_lines) {
        g_scroll_line = line - visible_lines + 1U;
    }
}

static void draw_editor(void) {
    const uint32_t text_top = USER_CLIENT_CHROME_HEIGHT;
    const uint32_t status_height = 32U;
    uint32_t columns = g_target_width > 20U ?
        (g_target_width - 20U) / FONT12X24_WIDTH : 1U;
    uint32_t visible_lines = g_target_height > text_top + status_height ?
        (g_target_height - text_top - status_height) / FONT12X24_HEIGHT : 1U;
    uint32_t line = 0U;
    uint32_t column = 0U;
    uint32_t cursor_line;
    uint32_t cursor_column;
    if (g_follow_cursor) ensure_cursor_visible(columns, visible_lines);
    cursor_position(columns, &cursor_line, &cursor_column);
    fill_rect(0U, 0U, g_target_width, g_target_height, 0x0015222AU);
    fill_rect(0U, 0U, g_target_width, USER_CLIENT_CHROME_HEIGHT,
              USER_CLIENT_CHROME_BACKGROUND);
    fill_rect(0U, USER_CLIENT_CHROME_HEIGHT - 1U, g_target_width, 1U,
              USER_CLIENT_CHROME_SEPARATOR);
    draw_text(64U, 16U, "NOTEPAD", USER_CLIENT_CHROME_TEXT);
    draw_text(160U, 16U, g_dirty ? "*" : " ", USER_CLIENT_CHROME_TEXT);
    draw_text(178U, 16U, g_file_path, USER_CLIENT_CHROME_TEXT);
    for (size_t index = 0U; index < g_text_length; ++index) {
        char character = g_text[index];
        if (character != '\n' && character != '\t' && line >= g_scroll_line &&
            line < g_scroll_line + visible_lines) {
            draw_character(10U + column * FONT12X24_WIDTH,
                           text_top + (line - g_scroll_line) * FONT12X24_HEIGHT,
                           character, 0x00D9EEF2U);
        }
        if (character == '\t' && line >= g_scroll_line &&
            line < g_scroll_line + visible_lines) {
            uint32_t spaces = 4U - (column & 3U);
            for (uint32_t space = 0U; space < spaces && column + space < columns; ++space) {
                draw_character(10U + (column + space) * FONT12X24_WIDTH,
                               text_top + (line - g_scroll_line) * FONT12X24_HEIGHT,
                               ' ', 0x00D9EEF2U);
            }
        }
        advance_position(character, columns, &line, &column);
    }
    if (cursor_line >= g_scroll_line && cursor_line < g_scroll_line + visible_lines &&
        cursor_column < columns) {
        fill_rect(10U + cursor_column * FONT12X24_WIDTH,
                  text_top + (cursor_line - g_scroll_line) * FONT12X24_HEIGHT,
                  2U, FONT12X24_HEIGHT, 0x00F2FFF9U);
    }
    fill_rect(0U, g_target_height - status_height, g_target_width,
              status_height, 0x00102028U);
    draw_text(10U, g_target_height - 28U, g_status, 0x008FD6C4U);
    user_client_chrome_close(g_target, g_target_width,
                             g_target_width, g_target_height,
                             USER_CLIENT_CHROME_HEIGHT,
                             USER_CLIENT_CHROME_CLOSE_BG,
                             USER_CLIENT_CHROME_CLOSE_FG);
}

static bool create_window(void) {
    os_display_info_t display = {0};
    os_window_create_t request = {0};
    uint32_t width;
    uint32_t height;
    display.hdr.size = sizeof(display);
    display.hdr.version = OS_SYSCALL_ABI_VERSION;
    if (notepad_syscall_one(OS_SYS_DISPLAY_GET_INFO, (uint64_t)&display) < 0 ||
        display.width < 320U || display.height < 240U) return false;
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    width = display.width * 3U / 4U;
    height = display.height * 3U / 4U;
    if (width < 640U && display.width > 640U) width = 640U;
    if (height < 420U && display.height > 420U) height = 420U;
    request.x = (int32_t)((display.width - width) / 2U);
    request.y = (int32_t)((display.height - height) / 2U);
    request.width = width;
    request.height = height;
    request.flags = OS_WINDOW_VISIBLE |
                    OS_WINDOW_RESIZABLE |
                    OS_WINDOW_CLIENT_DECORATIONS;
    request.background = 0x0015222AU;
    request.title[0] = 'N'; request.title[1] = 'O'; request.title[2] = 'T';
    request.title[3] = 'E'; request.title[4] = 'P'; request.title[5] = 'A';
    request.title[6] = 'D';
    request.address = NOTEPAD_MAP_BASE;
    if (notepad_syscall_one(OS_SYS_WINDOW_CREATE, (uint64_t)&request) != 0 ||
        request.window == OS_INVALID_HANDLE || request.address == 0U) return false;
    g_window.handle = request.window;
    g_window.identifier = request.identifier;
    g_window.width = request.width;
    g_window.height = request.height;
    g_window.pixels = (uint32_t *)(uintptr_t)request.address;
    return true;
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
        (void)notepad_syscall_one(OS_SYS_WINDOW_UPDATE, (uint64_t)&request);
    } else {
        for (uint32_t index = 0U; index < g_damage_count; ++index) {
            request.x = (int32_t)g_damage[index].x;
            request.y = (int32_t)g_damage[index].y;
            request.width = g_damage[index].width;
            request.height = g_damage[index].height;
            (void)notepad_syscall_one(OS_SYS_WINDOW_UPDATE,
                                      (uint64_t)&request);
        }
    }
    notepad_damage_reset();
}

static void render(void) {
    g_target = g_window.pixels;
    g_target_width = g_window.width;
    g_target_height = g_window.height;
    if (g_target == 0) return;
    if (!g_damage_full && g_damage_count == 0U) notepad_damage_all();
    draw_editor();
    update_window();
}

static bool load_document(void) {
    os_handle_t handle = OS_INVALID_HANDLE;
    uint64_t total = 0U;
    bool success = true;
    if (notepad_syscall_four(OS_SYS_FILE_OPEN, (uint64_t)g_file_path,
                             OS_FILE_OPEN_READ, 0U,
                             (uint64_t)&handle) < 0 || handle == OS_INVALID_HANDLE) {
        g_text[0] = '\0';
        g_text_length = 0U;
        g_cursor = 0U;
        history_reset();
        mark_saved();
        set_status("NEW DOCUMENT - CTRL+S TO SAVE");
        return false;
    }
    while (total + 1U < NOTEPAD_TEXT_CAPACITY) {
        uint64_t bytes = 0U;
        uint64_t capacity = NOTEPAD_TEXT_CAPACITY - 1U - total;
        if (capacity > 256U) capacity = 256U;
        int64_t status = notepad_syscall_four(OS_SYS_FILE_READ, handle,
                                              (uint64_t)(g_text + total), capacity,
                                              (uint64_t)&bytes);
        if (status < 0 || bytes > capacity) {
            success = false;
            set_status("LOAD FAILED");
            break;
        }
        total += bytes;
        if (bytes == 0U || bytes < capacity) break;
    }
    g_text_length = (size_t)total;
    g_cursor = g_text_length;
    g_scroll_line = 0U;
    g_follow_cursor = true;
    g_text[g_text_length] = '\0';
    (void)notepad_syscall_one(OS_SYS_HANDLE_CLOSE, handle);
    if (!success) {
        g_text[0] = '\0';
        g_text_length = 0U;
        g_cursor = 0U;
    }
    history_reset();
    mark_saved();
    if (success) set_status("CTRL+S SAVE  CTRL+Z UNDO  CTRL+Q EXIT");
    return success;
}

static void save_document(void) {
    os_handle_t handle = OS_INVALID_HANDLE;
    uint64_t total = 0U;
    if (notepad_syscall_four(OS_SYS_FILE_OPEN, (uint64_t)g_file_path,
                             OS_FILE_OPEN_READ | OS_FILE_OPEN_WRITE |
                             OS_FILE_OPEN_CREATE | OS_FILE_OPEN_TRUNCATE, 0U,
                             (uint64_t)&handle) < 0 || handle == OS_INVALID_HANDLE) {
        set_status("SAVE FAILED: FILE IS NOT WRITABLE");
        return;
    }
    while (total < g_text_length) {
        uint64_t bytes = 0U;
        uint64_t capacity = g_text_length - total;
        if (capacity > 256U) capacity = 256U;
        int64_t status = notepad_syscall_four(OS_SYS_FILE_WRITE, handle,
                                              (uint64_t)(g_text + total), capacity,
                                              (uint64_t)&bytes);
        if (status < 0 || bytes == 0U || bytes > capacity) {
            set_status("SAVE FAILED");
            (void)notepad_syscall_one(OS_SYS_HANDLE_CLOSE, handle);
            return;
        }
        total += bytes;
    }
    if (notepad_syscall_one(OS_SYS_FILE_FSYNC, handle) < 0) {
        set_status("SAVE FAILED: FLUSH ERROR");
        (void)notepad_syscall_one(OS_SYS_HANDLE_CLOSE, handle);
        return;
    }
    (void)notepad_syscall_one(OS_SYS_HANDLE_CLOSE, handle);
    mark_saved();
    set_status("SAVED");
}

static void new_document(void) {
    if (g_dirty) {
        set_status("UNSAVED CHANGES - CTRL+S FIRST");
        return;
    }
    if (g_text_length != 0U) begin_edit();
    g_text[0] = '\0';
    g_text_length = 0U;
    g_cursor = 0U;
    g_scroll_line = 0U;
    g_follow_cursor = true;
    g_dirty = true;
    set_status("NEW DOCUMENT - CTRL+S TO SAVE");
}

static void reload_document(void) {
    if (g_dirty) {
        set_status("UNSAVED CHANGES - CTRL+S FIRST");
        return;
    }
    if (load_document()) set_status("RELOADED");
}

static char key_to_ascii(uint32_t code, bool shift) {
    static const char lower[] = "abcdefghijklmnopqrstuvwxyz";
    static const char upper[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    if (code >= 'A' && code <= 'Z') return shift ? (char)code : (char)(code - 'A' + 'a');
    if (code >= 'a' && code <= 'z') return shift ? (char)(code - 'a' + 'A') : (char)code;
    if (code >= 0x04U && code <= 0x1DU) return shift ? upper[code - 0x04U] : lower[code - 0x04U];
    if (code >= 0x1EU && code <= 0x26U) {
        static const char numbers[] = "123456789";
        static const char symbols[] = "!@#$%^&*(";
        return shift ? symbols[code - 0x1EU] : numbers[code - 0x1EU];
    }
    if (code == 0x27U) return shift ? ')' : '0';
    if (code == 0x2CU) return ' ';
    if (code == 0x2DU) return shift ? '_' : '-';
    if (code == 0x2EU) return shift ? '+' : '=';
    if (code == 0x2FU) return shift ? '{' : '[';
    if (code == 0x30U) return shift ? '}' : ']';
    if (code == 0x31U) return shift ? '|' : '\\';
    if (code == 0x33U) return shift ? ':' : ';';
    if (code == 0x34U) return shift ? '"' : '\'';
    if (code == 0x35U) return shift ? '~' : '`';
    if (code == 0x36U) return shift ? '<' : ',';
    if (code == 0x37U) return shift ? '>' : '.';
    if (code == 0x38U) return shift ? '?' : '/';
    return '\0';
}

static void insert_character(char character) {
    if (g_text_length + 1U >= NOTEPAD_TEXT_CAPACITY) {
        set_status("DOCUMENT IS FULL");
        return;
    }
    begin_edit();
    for (size_t index = g_text_length + 1U; index > g_cursor; --index) {
        g_text[index] = g_text[index - 1U];
    }
    g_text[g_cursor++] = character;
    ++g_text_length;
    g_text[g_text_length] = '\0';
    g_dirty = true;
}

static void backspace(void) {
    if (g_cursor == 0U) return;
    begin_edit();
    for (size_t index = g_cursor; index <= g_text_length; ++index) {
        g_text[index - 1U] = g_text[index];
    }
    --g_cursor;
    --g_text_length;
    g_dirty = true;
}

static void delete_character(void) {
    if (g_cursor >= g_text_length) return;
    begin_edit();
    for (size_t index = g_cursor; index < g_text_length; ++index) {
        g_text[index] = g_text[index + 1U];
    }
    --g_text_length;
    g_dirty = true;
}

static void move_vertical_by(int32_t direction, uint32_t distance) {
    uint32_t columns = g_window.width > 20U ?
        (g_window.width - 20U) / FONT12X24_WIDTH : 1U;
    uint32_t line = 0U;
    uint32_t column = 0U;
    cursor_position(columns, &line, &column);
    if (direction < 0) {
        if (distance > line) line = 0U;
        else line -= distance;
    } else {
        if (distance > UINT32_MAX - line) line = UINT32_MAX;
        else line += distance;
    }
    g_cursor = offset_at_position(columns, line, column);
}

static void move_vertical(int32_t direction) {
    move_vertical_by(direction, 1U);
}

static void scroll_editor(int32_t direction) {
    uint32_t columns = g_window.width > 20U ?
        (g_window.width - 20U) / FONT12X24_WIDTH : 1U;
    uint32_t visible_lines =
        g_window.height > USER_CLIENT_CHROME_HEIGHT + 32U ?
        (g_window.height - USER_CLIENT_CHROME_HEIGHT - 32U) /
        FONT12X24_HEIGHT : 1U;
    uint32_t line = 0U;
    uint32_t column = 0U;
    uint32_t total_lines;
    uint32_t maximum;
    for (size_t index = 0U; index < g_text_length; ++index) {
        advance_position(g_text[index], columns, &line, &column);
    }
    total_lines = line == UINT32_MAX ? UINT32_MAX : line + 1U;
    maximum = total_lines > visible_lines ? total_lines - visible_lines : 0U;
    if (direction < 0) {
        uint32_t distance = (uint32_t)(-direction);
        g_scroll_line = distance > g_scroll_line ? 0U : g_scroll_line - distance;
    } else if ((uint32_t)direction > maximum -
               (g_scroll_line < maximum ? g_scroll_line : maximum)) {
        g_scroll_line = maximum;
    } else {
        g_scroll_line += (uint32_t)direction;
    }
    if (g_scroll_line > maximum) g_scroll_line = maximum;
    g_follow_cursor = false;
}

static void page_editor(int32_t direction) {
    uint32_t page = g_window.height > USER_CLIENT_CHROME_HEIGHT + 32U ?
        (g_window.height - USER_CLIENT_CHROME_HEIGHT - 32U) /
        FONT12X24_HEIGHT : 1U;
    if (page > 1000000000U) page = 1000000000U;
    scroll_editor(direction < 0 ? -(int32_t)page : (int32_t)page);
}

static bool key_matches(const os_input_event_t *input, uint32_t hid_code,
                        char ascii_code) {
    return input != 0 &&
           (input->code == hid_code || input->code == (uint32_t)ascii_code);
}

static bool handle_key(const os_window_event_t *event) {
    const os_input_event_t *input = event != 0 ? &event->input : 0;
    char character;
    bool dirty_before;
    if (event == 0 || event->type != OS_WINDOW_EVENT_INPUT ||
        input == 0 || input->type != OS_INPUT_EVENT_KEY) return false;
    if (input->code == 0xE1U || input->code == 0xE5U) {
        g_shift = input->value != OS_INPUT_VALUE_RELEASE;
        return false;
    }
    if (input->code == 0xE0U || input->code == 0xE4U) {
        g_ctrl = input->value != OS_INPUT_VALUE_RELEASE;
        return false;
    }
    if (input->value == OS_INPUT_VALUE_RELEASE) return false;
    dirty_before = g_dirty;
    g_follow_cursor = true;
    if (g_ctrl && key_matches(input, 0x16U, 'S')) {
        save_document();
    } else if (g_ctrl && key_matches(input, 0x14U, 'Q')) {
        notepad_exit(0U);
    } else if (g_ctrl && key_matches(input, 0x1DU, 'Z')) {
        undo_document();
    } else if (g_ctrl && key_matches(input, 0x1CU, 'Y')) {
        redo_document();
    } else if (g_ctrl && key_matches(input, 0x11U, 'N')) {
        new_document();
    } else if (g_ctrl && key_matches(input, 0x12U, 'O')) {
        reload_document();
    } else if (g_ctrl && input->code == 0x4AU) {
        g_cursor = 0U;
    } else if (g_ctrl && input->code == 0x4DU) {
        g_cursor = g_text_length;
    } else if (input->code == 0x4FU) {
        if (g_cursor < g_text_length) ++g_cursor;
    } else if (input->code == 0x50U) {
        if (g_cursor != 0U) --g_cursor;
    } else if (input->code == 0x52U) {
        move_vertical(-1);
    } else if (input->code == 0x51U) {
        move_vertical(1);
    } else if (input->code == 0x4BU) {
        page_editor(-1);
    } else if (input->code == 0x4EU) {
        page_editor(1);
    } else if (input->code == 0x4AU) {
        while (g_cursor != 0U && g_text[g_cursor - 1U] != '\n') --g_cursor;
    } else if (input->code == 0x4DU) {
        while (g_cursor < g_text_length && g_text[g_cursor] != '\n') ++g_cursor;
    } else if (input->code == 0x4CU) {
        delete_character();
    } else if (input->code == 0x2AU) {
        backspace();
    } else if (input->code == 0x28U || input->code == 0x58U) {
        insert_character('\n');
    } else if (input->code == 0x2BU) {
        insert_character('\t');
    } else if (!g_ctrl) {
        character = key_to_ascii(input->code, g_shift);
        if (character != '\0') insert_character(character);
    }
    notepad_damage_editor();
    if (g_ctrl || g_dirty != dirty_before) {
        notepad_damage_title();
        notepad_damage_status();
    }
    render();
    return true;
}

static bool handle_event(const os_window_event_t *event) {
    const os_input_event_t *input;
    uint64_t pixels;

    if (event == 0) return false;

    /* V2.3 cooperative close */
    if (event->type ==
        OS_WINDOW_EVENT_CLOSE_REQUEST) {

        /*
         * Do not silently discard an edited document.
         *
         * A real modal confirmation dialog can replace this later.
         */
        if (g_dirty) {
            set_status(
                "UNSAVED CHANGES - CTRL+S SAVE OR CTRL+Q EXIT");
            notepad_damage_status();
            render();
            return true;
        }

        notepad_exit(0U);
    }

    if (event->type == OS_WINDOW_EVENT_RESIZE) {
        if (event->resize.width == 0U || event->resize.height == 0U) return false;
        pixels = (uint64_t)event->resize.width * event->resize.height;
        if (pixels > event->resize.buffer_size / sizeof(uint32_t)) return false;
        g_window.width = event->resize.width;
        g_window.height = event->resize.height;
        g_follow_cursor = true;
        notepad_damage_all();
        render();
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
        if (g_dirty) {
            set_status(
                "UNSAVED CHANGES - CTRL+S SAVE OR CTRL+Q EXIT");
            notepad_damage_status();
            render();
            return true;
        }
        notepad_exit(0U);
    }
    if (input->type == OS_INPUT_EVENT_RELATIVE &&
        input->code == OS_INPUT_REL_WHEEL && input->value != 0) {
        scroll_editor(input->value > 0 ? -3 : 3);
        notepad_damage_editor();
        render();
        return true;
    }
    return handle_key(event);
}

int main(int argc, char **argv) {
    if (argc > 1 && (argv == 0 || argv[1] == 0 || !set_file_path(argv[1]))) {
        notepad_exit(1U);
    }
    if (!create_window()) notepad_exit(1U);
    (void)load_document();
    render();
    for (;;) {
        os_window_event_read_t request = {0};
        request.hdr.size = sizeof(request);
        request.hdr.version = OS_SYSCALL_ABI_VERSION;
        request.identifier = g_window.identifier;
        request.timeout_ns = NOTEPAD_EVENT_TIMEOUT;
        int64_t status = notepad_syscall_one(OS_SYS_WINDOW_EVENT_READ,
                                             (uint64_t)&request);
        if (status == 0) {
            (void)handle_event(&request.event);
        } else if (status != -11 && status != -110) {
            __asm__ volatile ("pause");
        }
    }
}

__attribute__((noreturn)) void notepad_entry(void) {
    uintptr_t frame = (uintptr_t)__builtin_frame_address(0);
    uint64_t *initial_stack = (uint64_t *)(frame + sizeof(uint64_t));
    int argc = (int)initial_stack[0];
    char **argv = (char **)(initial_stack + 1U);
    int status = main(argc, argv);
    notepad_exit(status < 0 ? 1U : (uint64_t)status);
}
