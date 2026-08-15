#include <stdint.h>
#include <stdbool.h>

#include <uapi/all.h>

#define FILEMAN_MAP_BASE       0x0A000000ULL
#define FILEMAN_EVENT_TIMEOUT  100000000ULL
#define FILEMAN_PATH_CAPACITY  256U
#define FILEMAN_ENTRY_CAPACITY 64U

typedef struct fileman_window {
    os_handle_t handle;
    uint32_t identifier;
    uint32_t width;
    uint32_t height;
    uint32_t *pixels;
} fileman_window_t;

typedef struct fileman_entry {
    os_file_info_t info;
    char path[FILEMAN_PATH_CAPACITY];
} fileman_entry_t;

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
static const char g_notepad_path[] = "/sbin/notepad";
static const char g_notepad_name[] = "notepad";
static fileman_window_t g_window = {OS_INVALID_HANDLE, 0U, 0U, 0U, 0};
static fileman_entry_t g_entries[FILEMAN_ENTRY_CAPACITY];
static char g_path[FILEMAN_PATH_CAPACITY] = "/";
static char g_status[128] = "ENTER OPEN  BACKSPACE PARENT  DEL REMOVE  CTRL+Q EXIT";
static uint32_t g_entry_count;
static uint32_t g_selected;
static uint32_t g_first_entry;
static bool g_ctrl;
static uint32_t *g_target;
static uint32_t g_target_width;
static uint32_t g_target_height;

void __main(void) {
}

static int64_t fileman_syscall_one(uint64_t number, uint64_t arg0) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = arg0;
    __asm__ volatile ("syscall" : "+a"(rax), "+D"(rdi) :
                      : "rcx", "r11", "memory");
    return (int64_t)rax;
}

static int64_t fileman_syscall_three(uint64_t number, uint64_t arg0,
                                     uint64_t arg1, uint64_t arg2) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = arg0;
    register uint64_t rsi __asm__("rsi") = arg1;
    register uint64_t rdx __asm__("rdx") = arg2;
    __asm__ volatile ("syscall" : "+a"(rax), "+D"(rdi), "+S"(rsi),
                      "+d"(rdx) : : "rcx", "r11", "memory");
    return (int64_t)rax;
}

__attribute__((noreturn)) static void fileman_exit(uint64_t status) {
    (void)fileman_syscall_one(OS_SYS_THREAD_EXIT, status);
    for (;;) __asm__ volatile ("pause");
}

static uint32_t text_length(const char *text) {
    uint32_t length = 0U;
    if (text == 0) return 0U;
    while (text[length] != '\0') ++length;
    return length;
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

static void append_character(char *destination, uint32_t capacity, char value) {
    uint32_t length = text_length(destination);
    if (destination != 0 && length + 1U < capacity) {
        destination[length] = value;
        destination[length + 1U] = '\0';
    }
}

static void append_decimal(char *destination, uint32_t capacity, uint64_t value) {
    char digits[24];
    uint32_t count = 0U;
    if (destination == 0 || capacity == 0U) return;
    do {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0U && count < sizeof(digits));
    while (count != 0U) append_character(destination, capacity, digits[--count]);
}

static const uint8_t *glyph_for(char character) {
    if (character == ' ') return g_upper_font[0];
    if (character >= 'A' && character <= 'Z') {
        return g_upper_font[1U + (uint32_t)(character - 'A')];
    }
    if (character >= 'a' && character <= 'z') {
        return g_lower_font[(uint32_t)(character - 'a')];
    }
    if (character >= '0' && character <= '9') {
        return g_upper_font[27U + (uint32_t)(character - '0')];
    }
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
        const uint8_t *glyph = glyph_for(text[index]);
        for (uint32_t row = 0U; row < 7U; ++row) {
            for (uint32_t column = 0U; column < 5U; ++column) {
                if ((glyph[row] & (1U << (4U - column))) != 0U) {
                    fill_rect(x + index * 6U + column, y + row, 1U, 1U, color);
                }
            }
        }
    }
}

static bool set_path(const char *source) {
    uint32_t length = 0U;
    if (source == 0 || source[0] == '\0') return false;
    if (source[0] == '/') {
        while (source[length] != '\0') {
            if (++length >= FILEMAN_PATH_CAPACITY) return false;
        }
        for (uint32_t index = 0U; index <= length; ++index) g_path[index] = source[index];
        return true;
    }
    static const char prefix[] = "/";
    uint32_t prefix_length = text_length(prefix);
    while (source[length] != '\0') {
        if (prefix_length + length + 1U >= FILEMAN_PATH_CAPACITY) return false;
        ++length;
    }
    for (uint32_t index = 0U; index < prefix_length; ++index) g_path[index] = prefix[index];
    for (uint32_t index = 0U; index <= length; ++index) {
        g_path[prefix_length + index] = source[index];
    }
    return true;
}

static bool join_path(char *destination, const char *directory, const char *name) {
    uint32_t directory_length;
    uint32_t name_length;
    if (destination == 0 || directory == 0 || name == 0) return false;
    directory_length = text_length(directory);
    name_length = text_length(name);
    if (directory_length + name_length + 2U >= FILEMAN_PATH_CAPACITY) return false;
    copy_text(destination, FILEMAN_PATH_CAPACITY, directory);
    if (directory_length != 1U || directory[0] != '/') append_character(
        destination, FILEMAN_PATH_CAPACITY, '/');
    append_text(destination, FILEMAN_PATH_CAPACITY, name);
    return true;
}

static void parent_directory(void) {
    uint32_t length = text_length(g_path);
    if (length <= 1U) return;
    while (length > 1U && g_path[length - 1U] == '/') g_path[--length] = '\0';
    while (length > 1U && g_path[length - 1U] != '/') g_path[--length] = '\0';
    if (length > 1U) g_path[length - 1U] = '\0';
    if (g_path[0] == '\0') copy_text(g_path, sizeof(g_path), "/");
}

static int64_t stat_path(const char *path, os_file_info_t *info) {
    os_file_stat_t request = {0};
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.path = (uint64_t)(uintptr_t)path;
    int64_t status = fileman_syscall_one(OS_SYS_FILE_STAT, (uint64_t)&request);
    if (status == 0 && info != 0) *info = request.info;
    return status;
}

static bool load_directory(void) {
    os_file_info_t directory_info = {0};
    if (stat_path(g_path, &directory_info) < 0 ||
        directory_info.type != OS_FILE_TYPE_DIRECTORY) {
        copy_text(g_status, sizeof(g_status), "NOT A DIRECTORY");
        return false;
    }
    g_entry_count = 0U;
    for (uint32_t index = 0U; index < FILEMAN_ENTRY_CAPACITY; ++index) {
        os_file_enumerate_t request = {0};
        request.hdr.size = sizeof(request);
        request.hdr.version = OS_SYSCALL_ABI_VERSION;
        request.path = (uint64_t)(uintptr_t)g_path;
        request.index = index;
        int64_t status = fileman_syscall_one(OS_SYS_FILE_ENUMERATE,
                                             (uint64_t)&request);
        if (status == -2) break;
        if (status < 0) {
            copy_text(g_status, sizeof(g_status), "DIRECTORY READ FAILED");
            return false;
        }
        g_entries[g_entry_count].info = request.info;
        if (!join_path(g_entries[g_entry_count].path, g_path, request.info.name)) {
            copy_text(g_status, sizeof(g_status), "PATH TOO LONG");
            return false;
        }
        ++g_entry_count;
    }
    if (g_selected >= g_entry_count) g_selected = g_entry_count == 0U ? 0U : g_entry_count - 1U;
    g_first_entry = 0U;
    copy_text(g_status, sizeof(g_status),
              "ENTER OPEN  BACKSPACE PARENT  DEL REMOVE  CTRL+Q EXIT");
    return true;
}

static bool create_window(void) {
    os_display_info_t display = {0};
    os_window_create_t request = {0};
    display.hdr.size = sizeof(display);
    display.hdr.version = OS_SYSCALL_ABI_VERSION;
    if (fileman_syscall_one(OS_SYS_DISPLAY_GET_INFO, (uint64_t)&display) < 0 ||
        display.width < 320U || display.height < 240U) return false;
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.x = 36;
    request.y = 36;
    request.width = display.width > 72U ? display.width - 72U : display.width;
    request.height = display.height > 72U ? display.height - 72U : display.height;
    request.flags = OS_WINDOW_VISIBLE;
    request.background = 0x0015222AU;
    request.title[0] = 'F'; request.title[1] = 'I'; request.title[2] = 'L';
    request.title[3] = 'E'; request.title[4] = 'M'; request.title[5] = 'A';
    request.title[6] = 'N'; request.title[7] = '\0';
    request.address = FILEMAN_MAP_BASE;
    if (fileman_syscall_one(OS_SYS_WINDOW_CREATE, (uint64_t)&request) != 0 ||
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
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.identifier = g_window.identifier;
    (void)fileman_syscall_one(OS_SYS_WINDOW_UPDATE, (uint64_t)&request);
}

static void render(void) {
    uint32_t rows = g_window.height > 58U ? (g_window.height - 58U) / 12U : 1U;
    g_target = g_window.pixels;
    g_target_width = g_window.width;
    g_target_height = g_window.height;
    if (g_target == 0) return;
    if (g_selected < g_first_entry) g_first_entry = g_selected;
    if (g_selected >= g_first_entry + rows) g_first_entry = g_selected - rows + 1U;
    fill_rect(0U, 0U, g_target_width, g_target_height, 0x0015222AU);
    fill_rect(0U, 0U, g_target_width, 22U, 0x00223948U);
    draw_text(10U, 7U, "LITEOS FILEMAN", 0x00B9D7E8U);
    draw_text(120U, 7U, g_path, 0x008FD6C4U);
    for (uint32_t row = 0U; row < rows; ++row) {
        uint32_t index = g_first_entry + row;
        if (index >= g_entry_count) break;
        char line[FILEMAN_PATH_CAPACITY];
        line[0] = index == g_selected ? '>' : ' ';
        line[1] = ' ';
        line[2] = '\0';
        append_text(line, sizeof(line), g_entries[index].info.type ==
                    OS_FILE_TYPE_DIRECTORY ? "[DIR]  " : "[FILE] ");
        append_text(line, sizeof(line), g_entries[index].info.name);
        if (g_entries[index].info.type != OS_FILE_TYPE_DIRECTORY) {
            append_text(line, sizeof(line), "  ");
            append_decimal(line, sizeof(line), g_entries[index].info.size);
            append_text(line, sizeof(line), " B");
        }
        if (index == g_selected) {
            fill_rect(0U, 26U + row * 12U, g_target_width, 10U, 0x00314F5CU);
        }
        draw_text(8U, 28U + row * 12U, line, 0x00D9EEF2U);
    }
    fill_rect(0U, g_target_height - 22U, g_target_width, 22U, 0x00102028U);
    draw_text(8U, g_target_height - 14U, g_status, 0x008FD6C4U);
    update_window();
}

static void open_selected(void) {
    if (g_entry_count == 0U || g_selected >= g_entry_count) return;
    if (g_entries[g_selected].info.type == OS_FILE_TYPE_DIRECTORY) {
        copy_text(g_path, sizeof(g_path), g_entries[g_selected].path);
        g_selected = 0U;
        if (!load_directory()) copy_text(g_status, sizeof(g_status), "OPEN DIRECTORY FAILED");
        return;
    }
    char *arguments[3];
    arguments[0] = (char *)g_notepad_name;
    arguments[1] = g_entries[g_selected].path;
    arguments[2] = 0;
    int64_t status = fileman_syscall_three(OS_SYS_PROCESS_EXEC,
                                           (uint64_t)(uintptr_t)g_notepad_path,
                                           (uint64_t)(uintptr_t)arguments, 0U);
    if (status < 0) copy_text(g_status, sizeof(g_status), "NOTEPAD LAUNCH FAILED");
}

static void remove_selected(void) {
    if (g_entry_count == 0U || g_selected >= g_entry_count) return;
    os_file_path_op_t request = {0};
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.path = (uint64_t)(uintptr_t)g_entries[g_selected].path;
    if (fileman_syscall_one(OS_SYS_FILE_REMOVE, (uint64_t)&request) < 0) {
        copy_text(g_status, sizeof(g_status), "REMOVE FAILED (DIR MUST BE EMPTY)");
        return;
    }
    (void)load_directory();
}

static void handle_key(const os_window_event_t *event) {
    const os_input_event_t *input = event != 0 ? &event->input : 0;
    uint32_t rows = g_window.height > 58U ? (g_window.height - 58U) / 12U : 1U;
    if (input == 0 || input->type != OS_INPUT_EVENT_KEY) return;
    if (input->code == 0xE0U || input->code == 0xE4U) {
        g_ctrl = input->value != OS_INPUT_VALUE_RELEASE;
        return;
    }
    if (input->value == OS_INPUT_VALUE_RELEASE) return;
    if (g_ctrl && (input->code == 0x14U || input->code == (uint32_t)'Q')) {
        fileman_exit(0U);
    } else if (g_ctrl && (input->code == 0x15U || input->code == (uint32_t)'R')) {
        (void)load_directory();
    } else if (input->code == 0x52U) {
        if (g_selected != 0U) --g_selected;
    } else if (input->code == 0x51U) {
        if (g_selected + 1U < g_entry_count) ++g_selected;
    } else if (input->code == 0x4AU) {
        g_selected = 0U;
    } else if (input->code == 0x4DU) {
        if (g_entry_count != 0U) g_selected = g_entry_count - 1U;
    } else if (input->code == 0x4BU) {
        g_selected = g_selected > rows ? g_selected - rows : 0U;
    } else if (input->code == 0x4EU) {
        if (g_entry_count != 0U) {
            uint32_t next = g_selected + rows;
            g_selected = next < g_entry_count ? next : g_entry_count - 1U;
        }
    } else if (input->code == 0x28U || input->code == 0x58U) {
        open_selected();
    } else if (input->code == 0x2AU) {
        parent_directory();
        g_selected = 0U;
        (void)load_directory();
    } else if (input->code == 0x4CU) {
        remove_selected();
    } else if (input->code == 0x3EU) {
        (void)load_directory();
    }
    render();
}

int main(int argc, char **argv) {
    if (argc > 1 && (argv == 0 || argv[1] == 0 || !set_path(argv[1]))) {
        fileman_exit(1U);
    }
    if (!create_window()) fileman_exit(1U);
    if (!load_directory()) fileman_exit(1U);
    render();
    for (;;) {
        os_window_event_read_t request = {0};
        request.hdr.size = sizeof(request);
        request.hdr.version = OS_SYSCALL_ABI_VERSION;
        request.identifier = g_window.identifier;
        request.timeout_ns = FILEMAN_EVENT_TIMEOUT;
        int64_t status = fileman_syscall_one(OS_SYS_WINDOW_EVENT_READ,
                                             (uint64_t)&request);
        if (status == 0) handle_key(&request.event);
        else if (status != -11 && status != -110) __asm__ volatile ("pause");
    }
}

__attribute__((noreturn)) void fileman_entry(void) {
    uintptr_t frame = (uintptr_t)__builtin_frame_address(0);
    uint64_t *initial_stack = (uint64_t *)(frame + sizeof(uint64_t));
    int argc = (int)initial_stack[0];
    char **argv = (char **)(initial_stack + 1U);
    int status = main(argc, argv);
    fileman_exit(status < 0 ? 1U : (uint64_t)status);
}
