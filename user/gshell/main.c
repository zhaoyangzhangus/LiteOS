#include <stdint.h>
#include <stdbool.h>

#include <uapi/all.h>

#include "../font12x24.h"

#define SHELL_LINE_CAPACITY 256U
#define SHELL_OUTPUT_LINES 24U
#define SHELL_MAP_BASE     0x06000000ULL
#define SHELL_EVENT_TIMEOUT 100000000ULL
#define SHELL_CHILD_STACK_TOP 0x00007FFFFFEF0000ULL
#define SHELL_LAUNCH_SLOTS 16U
#define SHELL_HISTORY_LINES 16U
#define SHELL_MAX_ARGUMENTS 8U
#define SHELL_PATH_CAPACITY 256U

typedef struct shell_window {
    os_handle_t handle;
    uint32_t identifier;
    uint32_t width;
    uint32_t height;
    uint32_t *pixels;
    const char *title;
    uint32_t color;
} shell_window_t;

typedef struct shell_launch_context {
    char path[SHELL_PATH_CAPACITY];
    char argument_storage[SHELL_MAX_ARGUMENTS][SHELL_LINE_CAPACITY];
    char *arguments[SHELL_MAX_ARGUMENTS + 1U];
} shell_launch_context_t;

static const uint8_t g_font[37][7] = {
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

/* The original shell font only covered uppercase commands.  Keep the same
 * compact 5x7 raster format, but provide lowercase and shell punctuation so
 * the command line is an actual text console instead of an uppercase-only
 * status panel. */
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
    {4,4,4,4,4,0,4},       {10,10,0,0,0,0,0},       {10,31,10,10,31,10,0},
    {4,15,20,14,5,30,4},   {25,2,4,8,19,0,0},       {12,18,20,8,21,18,13},
    {4,4,0,0,0,0,0},       {2,4,8,8,8,4,2},         {8,4,2,2,2,4,8},
    {0,10,4,31,4,10,0},    {0,4,4,31,4,4,0},        {0,0,0,0,4,4,8},
    {0,0,0,31,0,0,0},       {0,0,0,0,0,4,0},         {1,2,4,8,16,0,0},
    {0,4,0,0,4,0,0},        {0,4,0,0,4,4,8},         {2,4,8,16,8,4,2},
    {0,0,31,0,31,0,0},      {8,4,2,1,2,4,8},        {14,17,1,2,4,0,4},
    {14,17,29,21,29,16,14}, {14,8,8,8,8,8,14},      {16,8,4,2,1,0,0},
    {14,2,2,2,2,2,14},      {4,10,17,0,0,0,0},      {0,0,0,0,0,0,31},
    {4,2,0,0,0,0,0},        {2,4,8,4,2,4,8},        {4,4,4,4,4,4,4},
    {8,4,2,4,8,4,2},        {0,0,9,18,0,0,0},
};

static const char g_symbol_chars[] = "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~";

static shell_window_t g_shell = {
    OS_INVALID_HANDLE, 0U, 0U, 0U, 0, "SHELL", 0x00192B3DU,
};
static char g_command[SHELL_LINE_CAPACITY];
static size_t g_command_length;
static size_t g_command_cursor;
static char g_output[SHELL_OUTPUT_LINES][SHELL_LINE_CAPACITY];
static uint32_t g_output_count;
static shell_launch_context_t g_launch_contexts[SHELL_LAUNCH_SLOTS];
static uint32_t g_launch_slot_count;
static char g_history[SHELL_HISTORY_LINES][SHELL_LINE_CAPACITY];
static uint32_t g_history_count;
static int32_t g_history_cursor = -1;
static char g_cwd[SHELL_PATH_CAPACITY] = "/";
static bool g_shift;
static bool g_ctrl;
static uint32_t g_display_width;
static uint32_t g_display_height;
static uint32_t *g_target;
static uint32_t g_target_width;
static uint32_t g_target_height;
static uint8_t g_file_buffer[128];
static char g_file_line[SHELL_LINE_CAPACITY];
static char g_display_line[SHELL_LINE_CAPACITY];

static int64_t stat_path(const char *path, os_file_info_t *info);
static bool file_exists(const char *path);

static int64_t gshell_syscall_one(uint64_t number, uint64_t arg0) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = arg0;
    __asm__ volatile ("syscall" : "+a"(rax), "+D"(rdi) :
                      : "rcx", "r11", "memory");
    return (int64_t)rax;
}

static int64_t gshell_syscall_two(uint64_t number, uint64_t arg0,
                                  uint64_t arg1) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = arg0;
    register uint64_t rsi __asm__("rsi") = arg1;
    __asm__ volatile ("syscall" : "+a"(rax), "+D"(rdi), "+S"(rsi) :
                      : "rcx", "r11", "memory");
    return (int64_t)rax;
}

static int64_t gshell_syscall_three(uint64_t number, uint64_t arg0,
                                    uint64_t arg1, uint64_t arg2) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = arg0;
    register uint64_t rsi __asm__("rsi") = arg1;
    register uint64_t rdx __asm__("rdx") = arg2;
    __asm__ volatile ("syscall" : "+a"(rax), "+D"(rdi), "+S"(rsi),
                      "+d"(rdx) : : "rcx", "r11", "memory");
    return (int64_t)rax;
}

static int64_t gshell_syscall_four(uint64_t number, uint64_t arg0,
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

static void gshell_exit(uint64_t status) {
    (void)gshell_syscall_one(OS_SYS_THREAD_EXIT, status);
    for (;;) __asm__ volatile ("pause");
}

static void copy_text(char *destination, const char *source) {
    uint32_t index = 0U;
    while (index + 1U < SHELL_LINE_CAPACITY && source[index] != '\0') {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = '\0';
}

static uint32_t text_length(const char *text) {
    uint32_t length = 0U;
    if (text == 0) return 0U;
    while (text[length] != '\0' && length + 1U < SHELL_LINE_CAPACITY) ++length;
    return length;
}

static void append_text(char *destination, uint32_t capacity, const char *source) {
    uint32_t length = 0U;
    uint32_t source_index = 0U;
    if (destination == 0 || source == 0 || capacity == 0U) return;
    while (length + 1U < capacity && destination[length] != '\0') ++length;
    while (length + 1U < capacity && source[source_index] != '\0') {
        destination[length++] = source[source_index++];
    }
    destination[length] = '\0';
}

static void append_character(char *destination, uint32_t capacity, char character) {
    uint32_t length = 0U;
    if (destination == 0 || capacity < 2U) return;
    while (length + 1U < capacity && destination[length] != '\0') ++length;
    if (length + 1U >= capacity) return;
    destination[length++] = character;
    destination[length] = '\0';
}

static void append_output(const char *text) {
    if (g_output_count < SHELL_OUTPUT_LINES) {
        copy_text(g_output[g_output_count++], text);
        return;
    }
    for (uint32_t index = 1U; index < SHELL_OUTPUT_LINES; ++index) {
        copy_text(g_output[index - 1U], g_output[index]);
    }
    copy_text(g_output[SHELL_OUTPUT_LINES - 1U], text);
}

static void clear_output(void) {
    g_output_count = 0U;
}

static __attribute__((unused)) const uint8_t *glyph_for(char character) {
    if (character >= 'a' && character <= 'z') {
        return g_lower_font[(uint32_t)(character - 'a')];
    }
    if (character == ' ') return g_font[0];
    if (character >= 'A' && character <= 'Z') return g_font[1U + (uint32_t)(character - 'A')];
    if (character >= '0' && character <= '9') return g_font[27U + (uint32_t)(character - '0')];
    for (uint32_t index = 0U; index + 1U < sizeof(g_symbol_chars); ++index) {
        if (g_symbol_chars[index] == character) return g_symbol_font[index];
    }
    return g_font[0];
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

static void build_prompt(char *buffer) {
    if (buffer == 0) return;
    buffer[0] = '\0';
    append_text(buffer, SHELL_LINE_CAPACITY, "LITEOS:");
    append_text(buffer, SHELL_LINE_CAPACITY, g_cwd);
    append_text(buffer, SHELL_LINE_CAPACITY, "# ");
}

static void draw_input_line(void) {
    uint32_t prompt_length;
    uint32_t line_length;
    uint32_t visible_characters;
    uint32_t view_start = 0U;
    uint32_t cursor_position;
    uint32_t input_y;

    build_prompt(g_display_line);
    prompt_length = text_length(g_display_line);
    append_text(g_display_line, SHELL_LINE_CAPACITY, g_command);
    line_length = text_length(g_display_line);
    visible_characters = g_target_width > 20U ?
        (g_target_width - 20U) / FONT12X24_WIDTH : 0U;
    if (visible_characters == 0U) return;
    if (line_length > visible_characters) view_start = line_length - visible_characters;
    input_y = g_target_height > FONT12X24_HEIGHT + 4U ?
        g_target_height - FONT12X24_HEIGHT - 4U : 0U;
    draw_text(10U, input_y, g_display_line + view_start, 0x007FE0AEU);

    cursor_position = prompt_length + (uint32_t)g_command_cursor;
    if (cursor_position >= view_start && cursor_position - view_start < visible_characters) {
        fill_rect(10U + (cursor_position - view_start) * FONT12X24_WIDTH,
                  input_y, 2U, FONT12X24_HEIGHT, 0x00E8FFF4U);
    }
}

static void draw_terminal(void) {
    uint32_t visible = g_target_height > 64U ?
        (g_target_height - 64U) / FONT12X24_HEIGHT : 0U;
    uint32_t first = g_output_count > visible ? g_output_count - visible : 0U;
    uint32_t y = 32U;

    fill_rect(0U, 0U, g_target_width, g_target_height, g_shell.color);
    draw_text(10U, 4U, "LITEOS GRAPHICAL SHELL", 0x00B9D7E8U);
    for (uint32_t index = first;
         index < g_output_count && y + FONT12X24_HEIGHT + 32U <= g_target_height;
         ++index, y += FONT12X24_HEIGHT) {
        draw_text(10U, y, g_output[index], 0x00C5EAF4U);
    }
    if (g_target_height >= FONT12X24_HEIGHT + 4U) draw_input_line();
}

static void update_window(void) {
    os_window_update_t request = {0};
    if (g_shell.identifier == 0U) return;
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.identifier = g_shell.identifier;
    request.width = g_shell.width;
    request.height = g_shell.height;
    (void)gshell_syscall_one(OS_SYS_WINDOW_UPDATE, (uint64_t)&request);
}

static void render_window(void) {
    g_target = g_shell.pixels;
    g_target_width = g_shell.width;
    g_target_height = g_shell.height;
    if (g_target == 0) return;
    draw_terminal();
    update_window();
}

static bool create_window(int32_t x, int32_t y, uint32_t width,
                          uint32_t height) {
    os_window_create_t request = {0};
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.x = x;
    request.y = y;
    request.width = width;
    request.height = height;
    request.flags = OS_WINDOW_VISIBLE | OS_WINDOW_RESIZABLE;
    request.background = g_shell.color;
    for (uint32_t i = 0U; i < 31U && g_shell.title[i] != '\0'; ++i) {
        request.title[i] = g_shell.title[i];
    }
    request.address = SHELL_MAP_BASE;
    if (gshell_syscall_one(OS_SYS_WINDOW_CREATE, (uint64_t)&request) != 0 ||
        request.window == OS_INVALID_HANDLE || request.address == 0U) return false;
    g_shell.handle = request.window;
    g_shell.identifier = request.identifier;
    g_shell.width = width;
    g_shell.height = height;
    g_shell.pixels = (uint32_t *)(uintptr_t)request.address;
    return true;
}

static bool setup_windows(void) {
    os_display_info_t info = {0};
    uint32_t shell_width;
    uint32_t shell_height;
    info.hdr.size = sizeof(info);
    info.hdr.version = OS_SYSCALL_ABI_VERSION;
    if (gshell_syscall_one(OS_SYS_DISPLAY_GET_INFO, (uint64_t)&info) < 0 ||
        info.width < 320U || info.height < 240U) return false;
    g_display_width = info.width;
    g_display_height = info.height;
    shell_width = info.width > 32U ? info.width - 32U : info.width;
    /* Window decorations are part of this client surface now.  The kernel
     * compositor only adds the 1px frame and rounded clipping. */
    shell_height = info.height > 32U ? info.height - 32U : info.height;
    if (shell_width < 240U) shell_width = 240U;
    if (shell_height < 120U) shell_height = 120U;
    if (!create_window(16, 16, shell_width, shell_height)) return false;
    return true;
}

static char key_to_ascii(uint32_t code, bool shift) {
    static const char letters[] = "abcdefghijklmnopqrstuvwxyz";
    static const char shifted_letters[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    if (code >= (uint32_t)'A' && code <= (uint32_t)'Z') {
        return shift ? (char)code : (char)(code - (uint32_t)'A' + (uint32_t)'a');
    }
    if (code >= (uint32_t)'a' && code <= (uint32_t)'z') {
        return shift ? (char)(code - (uint32_t)'a' + (uint32_t)'A') : (char)code;
    }
    if (code >= 0x04U && code <= 0x1DU) {
        return shift ? shifted_letters[code - 0x04U] : letters[code - 0x04U];
    }
    if (code >= 0x1EU && code <= 0x26U) {
        static const char numbers[] = "123456789";
        static const char shifted[] = "!@#$%^&*(";
        return shift ? shifted[code - 0x1EU] : numbers[code - 0x1EU];
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

static char lower_ascii(char character) {
    return character >= 'A' && character <= 'Z' ?
           (char)(character - 'A' + 'a') : character;
}

static bool text_equals_ignore_case(const char *left, const char *right) {
    uint32_t index = 0U;
    while (left[index] != '\0' || right[index] != '\0') {
        if (lower_ascii(left[index]) != lower_ascii(right[index])) return false;
        ++index;
    }
    return true;
}

static bool is_shell_space(char character) {
    return character == ' ' || character == '\t';
}

/* Tokenize in place.  Quoting is useful for builtins now and keeps the
 * command line compatible with a future argv-aware process_exec ABI. */
static bool shell_tokenize(char *line, char **arguments, uint32_t *count,
                           bool *background) {
    uint32_t read_index = 0U;
    uint32_t write_index = 0U;
    uint32_t argument_start = 0U;
    uint32_t argument_count = 0U;
    bool in_single_quote = false;
    bool in_double_quote = false;
    bool escaped = false;
    bool in_argument = false;

    if (line == 0 || arguments == 0 || count == 0 || background == 0) return false;
    *count = 0U;
    *background = false;
    while (line[read_index] != '\0') {
        char character = line[read_index++];
        if (escaped) {
            line[write_index++] = character;
            escaped = false;
            in_argument = true;
            continue;
        }
        if (character == '\\' && !in_single_quote) {
            escaped = true;
            in_argument = true;
            continue;
        }
        if (character == '\'' && !in_double_quote) {
            in_single_quote = !in_single_quote;
            in_argument = true;
            continue;
        }
        if (character == '"' && !in_single_quote) {
            in_double_quote = !in_double_quote;
            in_argument = true;
            continue;
        }
        if (in_single_quote || in_double_quote) {
            line[write_index++] = character;
            continue;
        }
        if (character == '&') {
            if (in_argument) {
                if (argument_count >= SHELL_MAX_ARGUMENTS) return false;
                line[write_index++] = '\0';
                arguments[argument_count++] = line + argument_start;
                in_argument = false;
            }
            *background = true;
            while (is_shell_space(line[read_index])) ++read_index;
            if (line[read_index] != '\0') return false;
            continue;
        }
        if (is_shell_space(character)) {
            if (in_argument) {
                if (argument_count >= SHELL_MAX_ARGUMENTS) return false;
                line[write_index++] = '\0';
                arguments[argument_count++] = line + argument_start;
                in_argument = false;
            }
            continue;
        }
        if (!in_argument) {
            argument_start = write_index;
            in_argument = true;
        }
        line[write_index++] = character;
    }
    if (escaped || in_single_quote || in_double_quote) return false;
    if (in_argument) {
        if (argument_count >= SHELL_MAX_ARGUMENTS) return false;
        line[write_index++] = '\0';
        arguments[argument_count++] = line + argument_start;
    }
    line[write_index] = '\0';
    *count = argument_count;
    return true;
}

static void remove_last_path_component(char *path) {
    uint32_t length = 0U;
    if (path == 0) return;
    while (path[length] != '\0') ++length;
    if (length <= 1U) {
        path[0] = '/';
        path[1] = '\0';
        return;
    }
    while (length > 1U && path[length - 1U] != '/') --length;
    path[length > 1U ? length - 1U : 1U] = '\0';
}

static bool resolve_path(const char *input, char *path) {
    char combined[SHELL_PATH_CAPACITY];
    uint32_t read_index = 0U;
    uint32_t write_index = 0U;

    if (input == 0 || input[0] == '\0' || path == 0) return false;
    combined[0] = '\0';
    if (input[0] == '/') {
        append_text(combined, SHELL_PATH_CAPACITY, input);
    } else {
        append_text(combined, SHELL_PATH_CAPACITY, g_cwd);
        if (combined[text_length(combined) - 1U] != '/') {
            append_character(combined, SHELL_PATH_CAPACITY, '/');
        }
        append_text(combined, SHELL_PATH_CAPACITY, input);
    }
    path[0] = '/';
    path[1] = '\0';
    write_index = 1U;
    while (combined[read_index] != '\0') {
        uint32_t segment_start;
        uint32_t segment_length;
        while (combined[read_index] == '/') ++read_index;
        if (combined[read_index] == '\0') break;
        segment_start = read_index;
        while (combined[read_index] != '\0' && combined[read_index] != '/') ++read_index;
        segment_length = read_index - segment_start;
        if (segment_length == 1U && combined[segment_start] == '.') continue;
        if (segment_length == 2U && combined[segment_start] == '.' &&
            combined[segment_start + 1U] == '.') {
            remove_last_path_component(path);
            write_index = text_length(path);
            continue;
        }
        if (text_length(path) > 1U) {
            append_character(path, SHELL_PATH_CAPACITY, '/');
            write_index = text_length(path);
        }
        if (text_length(path) + segment_length >= SHELL_PATH_CAPACITY) return false;
        for (uint32_t index = 0U; index < segment_length; ++index) {
            path[write_index] = combined[segment_start + index];
            ++write_index;
        }
        path[write_index] = '\0';
        write_index = text_length(path);
    }
    return path[0] != '\0';
}

static bool make_program_path(const char *argument, char *path) {
    static const char *const prefixes[] = { "/sbin/", "/bin/", "/native/", "/" };
    char candidate[SHELL_LINE_CAPACITY];
    bool has_slash = false;
    if (argument == 0 || argument[0] == '\0' || path == 0) return false;
    for (uint32_t index = 0U; argument[index] != '\0'; ++index) {
        if (argument[index] == '/') {
            has_slash = true;
            break;
        }
    }
    if (has_slash || argument[0] == '.') return resolve_path(argument, path);
    if (resolve_path(argument, candidate) && file_exists(candidate)) {
        copy_text(path, candidate);
        return true;
    }
    for (uint32_t prefix_index = 0U;
         prefix_index < sizeof(prefixes) / sizeof(prefixes[0]); ++prefix_index) {
        candidate[0] = '\0';
        append_text(candidate, SHELL_LINE_CAPACITY, prefixes[prefix_index]);
        for (uint32_t index = 0U; argument[index] != '\0'; ++index) {
            append_character(candidate, SHELL_LINE_CAPACITY,
                             lower_ascii(argument[index]));
        }
        if (file_exists(candidate)) {
            copy_text(path, candidate);
            return true;
        }
    }
    return false;
}

static void append_file_chunk(const uint8_t *buffer, uint64_t count) {
    uint32_t line_length = 0U;
    if (buffer == 0) return;
    for (uint64_t index = 0U; index < count; ++index) {
        uint8_t value = buffer[index];
        if (value == '\r') continue;
        if (value == '\n' || line_length + 1U >= SHELL_LINE_CAPACITY) {
            g_file_line[line_length] = '\0';
            append_output(g_file_line);
            line_length = 0U;
            if (value == '\n') continue;
        }
        if (value < 0x20U || value > 0x7EU) value = '.';
        g_file_line[line_length++] = (char)value;
    }
    if (line_length != 0U) {
        g_file_line[line_length] = '\0';
        append_output(g_file_line);
    }
}

static void command_cat(const char *argument) {
    char path[SHELL_LINE_CAPACITY];
    os_handle_t handle = OS_INVALID_HANDLE;
    uint64_t total = 0U;
    if (!resolve_path(argument, path)) {
        append_output("USAGE: CAT FILE");
        return;
    }
    if (gshell_syscall_four(OS_SYS_FILE_OPEN, (uint64_t)path,
                            OS_FILE_OPEN_READ, 0U,
                            (uint64_t)&handle) < 0 ||
        handle == OS_INVALID_HANDLE) {
        append_output("CAT: FILE NOT FOUND");
        return;
    }
    while (total < 2048U) {
        uint64_t bytes = 0U;
        uint64_t capacity = sizeof(g_file_buffer);
        int64_t status = gshell_syscall_four(OS_SYS_FILE_READ, handle,
                                             (uint64_t)g_file_buffer, capacity,
                                             (uint64_t)&bytes);
        if (status < 0) {
            append_output("CAT: READ FAILED");
            break;
        }
        if (bytes == 0U) break;
        if (bytes > capacity) bytes = capacity;
        append_file_chunk(g_file_buffer, bytes);
        total += bytes;
    }
    (void)gshell_syscall_one(OS_SYS_HANDLE_CLOSE, handle);
    if (total == 0U) append_output("CAT: EMPTY FILE");
}

static int64_t enumerate_directory(const char *path, uint32_t index,
                                   os_file_info_t *info) {
    os_file_enumerate_t request = {0};
    if (path == 0 || info == 0) return -22;
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.path = (uint64_t)(uintptr_t)path;
    request.index = index;
    int64_t status = gshell_syscall_one(OS_SYS_FILE_ENUMERATE,
                                        (uint64_t)&request);
    if (status == 0) *info = request.info;
    return status;
}

static int64_t stat_path(const char *path, os_file_info_t *info) {
    os_file_stat_t request = {0};
    if (path == 0 || info == 0) return -22;
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.path = (uint64_t)(uintptr_t)path;
    int64_t status = gshell_syscall_one(OS_SYS_FILE_STAT,
                                        (uint64_t)(uintptr_t)&request);
    if (status == 0) *info = request.info;
    return status;
}

static bool directory_exists(const char *path) {
    os_file_info_t info = {0};
    return stat_path(path, &info) == 0 && info.type == OS_FILE_TYPE_DIRECTORY;
}

static bool file_exists(const char *path) {
    os_file_info_t info = {0};
    return stat_path(path, &info) == 0 && info.type == OS_FILE_TYPE_REGULAR;
}

static void command_ls(const char *argument) {
    char directory[SHELL_LINE_CAPACITY];
    const char *requested = argument != 0 && argument[0] != '\0' ? argument : g_cwd;
    if (!resolve_path(requested, directory)) {
        append_output("LS: INVALID PATH");
        return;
    }
    if (!directory_exists(directory)) {
        if (file_exists(directory)) {
            append_output(directory);
        } else {
            append_output("LS: DIRECTORY NOT FOUND");
        }
        return;
    }
    append_output(directory);
    for (uint32_t index = 0U; index < 64U; ++index) {
        os_file_info_t info = {0};
        int64_t status = enumerate_directory(directory, index, &info);
        if (status == -2) break;
        if (status < 0) {
            append_output("LS: ENUMERATION FAILED");
            return;
        }
        g_display_line[0] = '\0';
        append_text(g_display_line, SHELL_LINE_CAPACITY, info.name);
        if (info.type == OS_FILE_TYPE_DIRECTORY) {
            append_character(g_display_line, SHELL_LINE_CAPACITY, '/');
        }
        append_output(g_display_line);
    }
}

static void launch_program_entry(uint64_t context_address) {
    shell_launch_context_t *context =
        (shell_launch_context_t *)(uintptr_t)context_address;
    int64_t status = context == 0 ? -22 :
        gshell_syscall_three(OS_SYS_PROCESS_EXEC,
                             (uint64_t)(uintptr_t)context->path,
                             (uint64_t)(uintptr_t)context->arguments, 0U);
    (void)gshell_syscall_one(OS_SYS_THREAD_EXIT, status < 0 ? 1U : 0U);
    for (;;) __asm__ volatile ("pause");
}

static void append_decimal(char *destination, uint32_t capacity, int64_t value) {
    char digits[24];
    uint64_t magnitude;
    uint32_t digit_count = 0U;
    if (destination == 0 || capacity == 0U) return;
    destination[0] = '\0';
    if (value < 0) {
        append_character(destination, capacity, '-');
        magnitude = (uint64_t)(-(value + 1)) + 1U;
    } else {
        magnitude = (uint64_t)value;
    }
    do {
        digits[digit_count++] = (char)('0' + magnitude % 10U);
        magnitude /= 10U;
    } while (magnitude != 0U && digit_count < sizeof(digits));
    while (digit_count != 0U) append_character(destination, capacity, digits[--digit_count]);
}

static void append_status_line(const char *prefix, int64_t value) {
    char number[32];
    g_display_line[0] = '\0';
    append_text(g_display_line, SHELL_LINE_CAPACITY, prefix);
    append_decimal(number, sizeof(number), value);
    append_text(g_display_line, SHELL_LINE_CAPACITY, number);
    append_output(g_display_line);
}

static bool launch_program(const char *path, char **program_arguments,
                           uint32_t argument_count, bool background) {
    os_handle_t process_handle = OS_INVALID_HANDLE;
    os_handle_t thread_handle = OS_INVALID_HANDLE;
    os_thread_create_t arguments = {0};
    shell_launch_context_t *context;
    int64_t status;
    uint32_t slot;
    if (path == 0 || path[0] == '\0' || program_arguments == 0 ||
        argument_count == 0U || argument_count > SHELL_MAX_ARGUMENTS ||
        g_launch_slot_count >= SHELL_LAUNCH_SLOTS) {
        return false;
    }
    slot = g_launch_slot_count;
    context = &g_launch_contexts[slot];
    copy_text(context->path, path);
    for (uint32_t index = 0U; index < argument_count; ++index) {
        copy_text(context->argument_storage[index], program_arguments[index]);
        context->arguments[index] = context->argument_storage[index];
    }
    context->arguments[argument_count] = 0;
    arguments.hdr.size = sizeof(arguments);
    arguments.hdr.version = OS_SYSCALL_ABI_VERSION;
    arguments.entry = (uint64_t)(uintptr_t)launch_program_entry;
    arguments.stack_top = SHELL_CHILD_STACK_TOP;
    arguments.argument = (uint64_t)(uintptr_t)context;
    status = gshell_syscall_two(OS_SYS_PROCESS_CREATE, 0U,
                                (uint64_t)&process_handle);
    if (status < 0 || process_handle == OS_INVALID_HANDLE) return false;
    status = gshell_syscall_three(OS_SYS_THREAD_CREATE, process_handle,
                                  (uint64_t)&arguments,
                                  (uint64_t)&thread_handle);
    if (status < 0 || thread_handle == OS_INVALID_HANDLE) {
        (void)gshell_syscall_one(OS_SYS_HANDLE_CLOSE, process_handle);
        return false;
    }
    (void)gshell_syscall_one(OS_SYS_HANDLE_CLOSE, thread_handle);
    ++g_launch_slot_count;
    if (background) {
        (void)gshell_syscall_one(OS_SYS_HANDLE_CLOSE, process_handle);
        return true;
    }
    {
        os_wait_result_t result = {0};
        status = gshell_syscall_three(OS_SYS_WAIT_ONE, process_handle,
                                      OS_WAIT_INFINITE, (uint64_t)&result);
        (void)gshell_syscall_one(OS_SYS_HANDLE_CLOSE, process_handle);
        if (status < 0) return false;
        append_status_line("PROGRAM EXIT STATUS ", result.value);
    }
    return true;
}

static void remember_history(void) {
    if (g_command_length == 0U) return;
    if (g_history_count != 0U &&
        text_equals_ignore_case(g_history[g_history_count - 1U], g_command)) return;
    if (g_history_count < SHELL_HISTORY_LINES) {
        copy_text(g_history[g_history_count++], g_command);
    } else {
        for (uint32_t index = 1U; index < SHELL_HISTORY_LINES; ++index) {
            copy_text(g_history[index - 1U], g_history[index]);
        }
        copy_text(g_history[SHELL_HISTORY_LINES - 1U], g_command);
    }
}

static void append_command_echo(void) {
    build_prompt(g_display_line);
    append_text(g_display_line, SHELL_LINE_CAPACITY, g_command);
    append_output(g_display_line);
}

static void append_history_line(uint32_t index) {
    char number[32];
    append_decimal(number, sizeof(number), (int64_t)(index + 1U));
    g_display_line[0] = '\0';
    append_text(g_display_line, SHELL_LINE_CAPACITY, number);
    append_text(g_display_line, SHELL_LINE_CAPACITY, "  ");
    append_text(g_display_line, SHELL_LINE_CAPACITY, g_history[index]);
    append_output(g_display_line);
}

static void command_echo(char **arguments, uint32_t count) {
    g_display_line[0] = '\0';
    for (uint32_t index = 1U; index < count; ++index) {
        if (index != 1U) append_character(g_display_line, SHELL_LINE_CAPACITY, ' ');
        append_text(g_display_line, SHELL_LINE_CAPACITY, arguments[index]);
    }
    append_output(g_display_line);
}

static void command_cd(char **arguments, uint32_t count) {
    char path[SHELL_LINE_CAPACITY];
    const char *target = count == 1U ? "/" : arguments[1];
    if (count > 2U) {
        append_output("USAGE: CD [DIRECTORY]");
        return;
    }
    if (!resolve_path(target, path) || !directory_exists(path)) {
        append_output("CD: DIRECTORY NOT FOUND");
        return;
    }
    copy_text(g_cwd, path);
}

static int64_t file_path_operation(uint64_t syscall_number, const char *path,
                                   uint32_t mode) {
    os_file_path_op_t request = {0};
    if (path == 0) return -22;
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.path = (uint64_t)(uintptr_t)path;
    request.mode = mode;
    return gshell_syscall_one(syscall_number, (uint64_t)(uintptr_t)&request);
}

static void command_stat(char **arguments, uint32_t count) {
    char path[SHELL_LINE_CAPACITY];
    os_file_info_t info = {0};
    if (count != 2U) {
        append_output("USAGE: STAT FILE");
        return;
    }
    if (!resolve_path(arguments[1], path) || stat_path(path, &info) < 0) {
        append_output("STAT: PATH NOT FOUND");
        return;
    }
    g_display_line[0] = '\0';
    append_text(g_display_line, SHELL_LINE_CAPACITY,
                info.type == OS_FILE_TYPE_DIRECTORY ? "TYPE DIRECTORY" : "TYPE FILE");
    append_output(g_display_line);
    g_display_line[0] = '\0';
    append_text(g_display_line, SHELL_LINE_CAPACITY, "SIZE ");
    append_decimal(g_display_line + 5U, SHELL_LINE_CAPACITY - 5U,
                   (int64_t)info.size);
    append_output(g_display_line);
}

static void command_mkdir(char **arguments, uint32_t count) {
    char path[SHELL_LINE_CAPACITY];
    if (count != 2U) {
        append_output("USAGE: MKDIR DIRECTORY");
        return;
    }
    if (!resolve_path(arguments[1], path) ||
        file_path_operation(OS_SYS_FILE_MKDIR, path, 0755U) < 0) {
        append_output("MKDIR: FAILED");
        return;
    }
    append_output("DIRECTORY CREATED");
}

static void command_touch(char **arguments, uint32_t count) {
    char path[SHELL_LINE_CAPACITY];
    os_handle_t handle = OS_INVALID_HANDLE;
    if (count != 2U) {
        append_output("USAGE: TOUCH FILE");
        return;
    }
    if (!resolve_path(arguments[1], path) ||
        gshell_syscall_four(OS_SYS_FILE_OPEN, (uint64_t)path,
                            OS_FILE_OPEN_READ | OS_FILE_OPEN_WRITE |
                            OS_FILE_OPEN_CREATE, 0666U, (uint64_t)&handle) < 0 ||
        handle == OS_INVALID_HANDLE) {
        append_output("TOUCH: FAILED");
        return;
    }
    (void)gshell_syscall_one(OS_SYS_HANDLE_CLOSE, handle);
    append_output("FILE READY");
}

static void command_remove(char **arguments, uint32_t count) {
    char path[SHELL_LINE_CAPACITY];
    if (count != 2U) {
        append_output("USAGE: RM PATH");
        return;
    }
    if (!resolve_path(arguments[1], path) ||
        file_path_operation(OS_SYS_FILE_REMOVE, path, 0U) < 0) {
        append_output("RM: FAILED");
        return;
    }
    append_output("REMOVED");
}

static bool is_builtin_command(const char *name) {
    static const char *const names[] = {
        "help", "about", "status", "version", "clear", "echo", "pwd",
        "cd", "ls", "dir", "cat", "stat", "mkdir", "touch", "rm",
        "history", "which", "type", "open", "run", "fileman", "fm",
        "notepad", "exit", "true", "false",
    };
    for (uint32_t index = 0U; index < sizeof(names) / sizeof(names[0]); ++index) {
        if (text_equals_ignore_case(name, names[index])) return true;
    }
    return false;
}

static void command_program(char **arguments, uint32_t count, bool background) {
    char path[SHELL_LINE_CAPACITY];
    if (count < 2U) {
        append_output("USAGE: OPEN PROGRAM [&]");
        return;
    }
    if (!make_program_path(arguments[1], path)) {
        append_output("PROGRAM NOT FOUND");
        return;
    }
    if (!launch_program(path, arguments + 1U, count - 1U, background)) {
        append_output("PROGRAM LAUNCH FAILED");
        return;
    }
    if (background) append_output("PROGRAM STARTED IN BACKGROUND");
}

static void command_external(char **arguments, uint32_t count, bool background) {
    char path[SHELL_LINE_CAPACITY];
    if (count == 0U) return;
    if (!make_program_path(arguments[0], path)) {
        append_output("COMMAND NOT FOUND");
        return;
    }
    if (!launch_program(path, arguments, count, background)) {
        append_output("PROGRAM LAUNCH FAILED");
        return;
    }
    if (background) append_output("PROGRAM STARTED IN BACKGROUND");
}

static void execute_command(void) {
    char *arguments[SHELL_MAX_ARGUMENTS];
    uint32_t count = 0U;
    bool background = false;
    bool parsed;

    if (g_command_length == 0U) {
        append_output("TYPE HELP FOR COMMANDS");
        goto reset_command;
    }
    remember_history();
    append_command_echo();
    parsed = shell_tokenize(g_command, arguments, &count, &background);
    if (!parsed) {
        append_output("SYNTAX ERROR: QUOTE OR ESCAPE NOT CLOSED");
        goto reset_command;
    }
    if (count == 0U) {
        append_output("TYPE HELP FOR COMMANDS");
    } else if (text_equals_ignore_case(arguments[0], "help")) {
        append_output("HELP ABOUT STATUS VERSION");
        append_output("CLEAR ECHO PWD CD LS CAT STAT MKDIR TOUCH RM");
        append_output("WHICH TYPE OPEN RUN NOTEPAD FILEMAN FM [ARGS]");
        append_output("QUOTES AND BACKSLASH ESCAPES ARE SUPPORTED");
    } else if (text_equals_ignore_case(arguments[0], "about")) {
        append_output("LITEOS GRAPHICAL SHELL");
        append_output("ONE WINDOW CLIENT COMPOSED BY KERNEL");
    } else if (text_equals_ignore_case(arguments[0], "status")) {
        append_output("WINDOW SERVER: KERNEL");
        append_output("WINDOW COUNT: ONE SHELL");
        append_output(g_cwd);
    } else if (text_equals_ignore_case(arguments[0], "version")) {
        append_output("LITEOS VERSION 1 ABI 1");
    } else if (text_equals_ignore_case(arguments[0], "clear")) {
        clear_output();
    } else if (text_equals_ignore_case(arguments[0], "echo")) {
        command_echo(arguments, count);
    } else if (text_equals_ignore_case(arguments[0], "pwd")) {
        append_output(g_cwd);
    } else if (text_equals_ignore_case(arguments[0], "cd")) {
        command_cd(arguments, count);
    } else if (text_equals_ignore_case(arguments[0], "ls") ||
               text_equals_ignore_case(arguments[0], "dir")) {
        if (count > 2U) append_output("USAGE: LS [DIRECTORY]");
        else command_ls(count == 2U ? arguments[1] : 0);
    } else if (text_equals_ignore_case(arguments[0], "cat")) {
        if (count != 2U) append_output("USAGE: CAT FILE");
        else command_cat(arguments[1]);
    } else if (text_equals_ignore_case(arguments[0], "stat")) {
        command_stat(arguments, count);
    } else if (text_equals_ignore_case(arguments[0], "mkdir")) {
        command_mkdir(arguments, count);
    } else if (text_equals_ignore_case(arguments[0], "touch")) {
        command_touch(arguments, count);
    } else if (text_equals_ignore_case(arguments[0], "rm")) {
        command_remove(arguments, count);
    } else if (text_equals_ignore_case(arguments[0], "history")) {
        for (uint32_t index = 0U; index < g_history_count; ++index) {
            append_history_line(index);
        }
    } else if (text_equals_ignore_case(arguments[0], "which") ||
               text_equals_ignore_case(arguments[0], "type")) {
        char path[SHELL_LINE_CAPACITY];
        if (count != 2U) {
            append_output("USAGE: WHICH COMMAND");
        } else if (is_builtin_command(arguments[1])) {
            g_display_line[0] = '\0';
            append_text(g_display_line, SHELL_LINE_CAPACITY, "BUILTIN ");
            append_text(g_display_line, SHELL_LINE_CAPACITY, arguments[1]);
            append_output(g_display_line);
        } else if (make_program_path(arguments[1], path)) {
            append_output(path);
        } else {
            append_output("COMMAND NOT FOUND");
        }
    } else if (text_equals_ignore_case(arguments[0], "open") ||
               text_equals_ignore_case(arguments[0], "run")) {
        bool open_command = text_equals_ignore_case(arguments[0], "open");
        command_program(arguments, count, open_command || background);
    } else if (text_equals_ignore_case(arguments[0], "true")) {
        /* A successful no-op is useful in scripts once command sequencing is added. */
    } else if (text_equals_ignore_case(arguments[0], "false")) {
        append_output("FALSE");
    } else if (text_equals_ignore_case(arguments[0], "exit")) {
        gshell_exit(0U);
    } else {
        command_external(arguments, count, background);
    }

reset_command:
    g_command_length = 0U;
    g_command_cursor = 0U;
    g_history_cursor = -1;
    g_command[0] = '\0';
}

static void clear_command_line(void) {
    g_command_length = 0U;
    g_command_cursor = 0U;
    g_command[0] = '\0';
}

static void load_history_entry(int32_t index) {
    if (index < 0 || (uint32_t)index >= g_history_count) return;
    copy_text(g_command, g_history[(uint32_t)index]);
    g_command_length = text_length(g_command);
    g_command_cursor = g_command_length;
}

static void history_previous(void) {
    if (g_history_count == 0U) return;
    if (g_history_cursor < 0) g_history_cursor = (int32_t)g_history_count - 1;
    else if (g_history_cursor > 0) --g_history_cursor;
    load_history_entry(g_history_cursor);
}

static void history_next(void) {
    if (g_history_cursor < 0) return;
    if ((uint32_t)(g_history_cursor + 1) >= g_history_count) {
        g_history_cursor = -1;
        clear_command_line();
        return;
    }
    ++g_history_cursor;
    load_history_entry(g_history_cursor);
}

static bool text_has_prefix_ignore_case(const char *text, const char *prefix) {
    uint32_t index = 0U;
    while (prefix[index] != '\0') {
        if (text[index] == '\0' || lower_ascii(text[index]) != lower_ascii(prefix[index])) {
            return false;
        }
        ++index;
    }
    return true;
}

static void insert_command_text(const char *text) {
    uint32_t length = text_length(text);
    if (text == 0 || length == 0U || g_command_length + length >= SHELL_LINE_CAPACITY) return;
    for (uint32_t index = (uint32_t)g_command_length + length;
         index >= g_command_cursor + length; --index) {
        g_command[index] = g_command[index - length];
    }
    for (uint32_t index = 0U; index < length; ++index) {
        g_command[g_command_cursor + index] = text[index];
    }
    g_command_length += length;
    g_command_cursor += length;
    g_command[g_command_length] = '\0';
}

static void insert_command_character(char character) {
    char text[2] = {character, '\0'};
    insert_command_text(text);
}

static void backspace_command_character(void) {
    if (g_command_cursor == 0U) return;
    for (uint32_t index = (uint32_t)g_command_cursor; index <= g_command_length; ++index) {
        g_command[index - 1U] = g_command[index];
    }
    --g_command_cursor;
    --g_command_length;
}

static void delete_command_character(void) {
    if (g_command_cursor >= g_command_length) return;
    for (uint32_t index = (uint32_t)g_command_cursor; index < g_command_length; ++index) {
        g_command[index] = g_command[index + 1U];
    }
    --g_command_length;
}

static void complete_command(void) {
    static const char *const candidates[] = {
        "help", "about", "status", "version", "clear", "echo", "pwd", "cd",
        "ls", "dir", "cat", "history", "which", "type", "open", "run", "exit",
        "true", "false", "deviced", "logd", "crashd", "audiod", "gshell",
        "notepad", "fileman", "fm", "netmgr",
    };
    uint32_t start = (uint32_t)g_command_cursor;
    uint32_t matches = 0U;
    const char *match = 0;
    if (g_command_cursor != g_command_length) return;
    while (start != 0U && !is_shell_space(g_command[start - 1U])) --start;
    if (start != 0U) return;
    g_command[g_command_cursor] = '\0';
    for (uint32_t index = 0U; index < sizeof(candidates) / sizeof(candidates[0]); ++index) {
        if (text_has_prefix_ignore_case(candidates[index], g_command + start)) {
            ++matches;
            match = candidates[index];
        }
    }
    if (matches == 1U && match != 0) {
        insert_command_text(match + g_command_cursor - start);
    } else if (matches > 1U) {
        append_output("MATCHES:");
        for (uint32_t index = 0U; index < sizeof(candidates) / sizeof(candidates[0]); ++index) {
            if (text_has_prefix_ignore_case(candidates[index], g_command + start)) {
                append_output(candidates[index]);
            }
        }
    }
}

static bool handle_key(const os_window_event_t *event) {
    const os_input_event_t *input = event != 0 ? &event->input : 0;
    char character;
    /* EVENT_READ 已按 g_shell.identifier 过滤；不要因某个输入后端未回填
     * event.identifier 而丢弃本来属于 shell 的按键。 */
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
    if (g_ctrl && (input->code == 0x14U || input->code == (uint32_t)'Q')) {
        gshell_exit(0U);
    } else if (g_ctrl &&
               (input->code == 0x04U || input->code == (uint32_t)'A')) {
        g_command_cursor = 0U;
    } else if (g_ctrl && (input->code == 0x08U || input->code == (uint32_t)'E')) {
        g_command_cursor = g_command_length;
    } else if (g_ctrl && (input->code == 0x06U || input->code == (uint32_t)'C')) {
        if (g_command_length != 0U) append_output("^C");
        clear_command_line();
    } else if (g_ctrl && (input->code == 0x0FU || input->code == (uint32_t)'L')) {
        clear_output();
    } else if (g_ctrl && (input->code == 0x18U || input->code == (uint32_t)'U')) {
        clear_command_line();
    } else if (g_ctrl && (input->code == 0x07U || input->code == (uint32_t)'G')) {
        delete_command_character();
    } else if (input->code == 0x28U || input->code == 0x58U) {
        execute_command();
    } else if (input->code == 0x2AU) {
        backspace_command_character();
    } else if (input->code == 0x4FU) {
        if (g_command_cursor < g_command_length) ++g_command_cursor;
    } else if (input->code == 0x50U) {
        if (g_command_cursor != 0U) --g_command_cursor;
    } else if (input->code == 0x4AU) {
        g_command_cursor = 0U;
    } else if (input->code == 0x4DU) {
        g_command_cursor = g_command_length;
    } else if (input->code == 0x4CU) {
        delete_command_character();
    } else if (input->code == 0x52U) {
        history_previous();
    } else if (input->code == 0x51U) {
        history_next();
    } else if (input->code == 0x2BU) {
        complete_command();
    } else if (!g_ctrl) {
        character = key_to_ascii(input->code, g_shift);
        if (character != '\0') insert_command_character(character);
    }
    render_window();
    return true;
}

static bool handle_event(const os_window_event_t *event) {
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
    if (!setup_windows()) gshell_exit(1U);
    append_output("LITEOS GRAPHICAL SHELL READY");
    append_output("WINDOW CONTENT IS COMPOSITED BY KERNEL");
    append_output("TYPE HELP FOR COMMANDS");
    render_window();
    for (;;) {
        os_window_event_read_t request = {0};
        int64_t status;
        request.hdr.size = sizeof(request);
        request.hdr.version = OS_SYSCALL_ABI_VERSION;
        request.identifier = g_shell.identifier;
        request.timeout_ns = SHELL_EVENT_TIMEOUT;
        status = gshell_syscall_one(OS_SYS_WINDOW_EVENT_READ, (uint64_t)&request);
        if (status == 0) {
            (void)handle_event(&request.event);
        } else if (status != -11 && status != -110) {
            __asm__ volatile ("pause");
        }
    }
}
