#include <stdbool.h>
#include <stdint.h>

#include <uapi/all.h>

#include "../../client_chrome.h"
#include "../../runtime/liteos_text.h"

#define TASKMGR_MAP_BASE       0x0E000000ULL
#define TASKMGR_EVENT_WAIT OS_WAIT_INFINITE
#define TASKMGR_MAX_PROCESSES   256U
#define TASKMGR_MAX_THREADS     512U
#define TASKMGR_MAX_ENTRIES     (TASKMGR_MAX_PROCESSES + TASKMGR_MAX_THREADS)
#define TASKMGR_ROW_HEIGHT      34U
#define TASKMGR_TABLE_TOP       116U
#define TASKMGR_STATUS_HEIGHT   32U
#define TASKMGR_STATUS_CAP      96U

#define TASKMGR_BACKGROUND      0x00F4F6F8U
#define TASKMGR_HEADER          0x00FAFAFAU
#define TASKMGR_TABLE_HEADER    0x00E9EDF1U
#define TASKMGR_TEXT            0x00353B43U
#define TASKMGR_MUTED           0x006A747CU
#define TASKMGR_ACCENT          0x005B86D6U
#define TASKMGR_SELECTED        0x00DCEBFAU

typedef struct taskmgr_window {
    os_handle_t handle;
    uint32_t identifier;
    uint32_t width;
    uint32_t height;
    uint32_t *pixels;
} taskmgr_window_t;

typedef struct taskmgr_entry {
    bool thread;
    uint64_t id;
    uint64_t parent_id;
    uint32_t state;
    uint32_t cpu;
    uint32_t thread_count;
    char name[OS_PROCESS_NAME_MAX];
} taskmgr_entry_t;

static taskmgr_window_t g_window = {
    OS_INVALID_HANDLE, 0U, 0U, 0U, 0,
};
static taskmgr_entry_t g_entries[TASKMGR_MAX_ENTRIES];
static uint32_t g_entry_count;
static uint32_t g_first_row;
static uint32_t g_selected_entry;
static bool g_selection_valid;
static bool g_entries_truncated;
static bool g_ctrl;
static char g_status[TASKMGR_STATUS_CAP] =
    "R REFRESH  CTRL +/- FONT  ESC CLOSE";

void __main(void) {
}

static int64_t taskmgr_syscall6(uint64_t number, uint64_t arg0,
                                uint64_t arg1, uint64_t arg2,
                                uint64_t arg3, uint64_t arg4,
                                uint64_t arg5) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = arg0;
    register uint64_t rsi __asm__("rsi") = arg1;
    register uint64_t rdx __asm__("rdx") = arg2;
    register uint64_t r10 __asm__("r10") = arg3;
    register uint64_t r8 __asm__("r8") = arg4;
    register uint64_t r9 __asm__("r9") = arg5;
    __asm__ volatile ("syscall" : "+a"(rax), "+D"(rdi), "+S"(rsi),
                      "+d"(rdx), "+r"(r10), "+r"(r8), "+r"(r9) :
                      : "rcx", "r11", "memory");
    return (int64_t)rax;
}

static int64_t taskmgr_syscall_one(uint64_t number, uint64_t arg0) {
    return taskmgr_syscall6(number, arg0, 0U, 0U, 0U, 0U, 0U);
}

__attribute__((noreturn)) static void taskmgr_exit(uint64_t status) {
    (void)taskmgr_syscall_one(OS_SYS_THREAD_EXIT, status);
    for (;;) __asm__ volatile ("pause");
}

static uint32_t text_length(const char *text) {
    uint32_t length = 0U;
    if (text == 0) return 0U;
    while (text[length] != '\0' && length + 1U < TASKMGR_STATUS_CAP) ++length;
    return length;
}

static void copy_text(char *destination, uint32_t capacity,
                      const char *source) {
    uint32_t index = 0U;
    if (destination == 0 || capacity == 0U) return;
    if (source != 0) {
        while (index + 1U < capacity && source[index] != '\0') {
            destination[index] = source[index];
            ++index;
        }
    }
    destination[index] = '\0';
}

static void append_text(char *destination, uint32_t capacity,
                        const char *source) {
    uint32_t length = text_length(destination);
    uint32_t index = 0U;
    if (destination == 0 || source == 0 || capacity == 0U) return;
    while (length + 1U < capacity && source[index] != '\0') {
        destination[length++] = source[index++];
    }
    destination[length] = '\0';
}

static void append_u64(char *destination, uint32_t capacity, uint64_t value) {
    char digits[20];
    uint32_t count = 0U;
    do {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0U && count < sizeof(digits));
    while (count != 0U) {
        char text[2] = {digits[--count], '\0'};
        append_text(destination, capacity, text);
    }
}

static void set_status(const char *text) {
    copy_text(g_status, sizeof(g_status), text);
}

static const char *process_state_text(uint32_t state) {
    if (state == OS_PROCESS_NEW) return "NEW";
    if (state == OS_PROCESS_RUNNING) return "RUNNING";
    if (state == OS_PROCESS_EXITING) return "EXITING";
    if (state == OS_PROCESS_DEAD) return "DEAD";
    return "UNKNOWN";
}

static const char *thread_state_text(uint32_t state) {
    if (state == OS_THREAD_NEW) return "NEW";
    if (state == OS_THREAD_READY) return "READY";
    if (state == OS_THREAD_RUNNING) return "RUNNING";
    if (state == OS_THREAD_BLOCKED) return "BLOCKED";
    if (state == OS_THREAD_STOPPED) return "STOPPED";
    if (state == OS_THREAD_DEAD) return "DEAD";
    return "UNKNOWN";
}

static void refresh_processes(void) {
    bool selected_thread = g_selection_valid &&
        g_selected_entry < g_entry_count && g_entries[g_selected_entry].thread;
    uint64_t selected_id = g_selection_valid && g_selected_entry < g_entry_count ?
                           g_entries[g_selected_entry].id : 0U;
    g_entry_count = 0U;
    g_entries_truncated = false;
    for (uint32_t index = 0U; index <= TASKMGR_MAX_PROCESSES; ++index) {
        os_process_enumerate_t request = {0};
        int64_t status;
        request.hdr.size = sizeof(request);
        request.hdr.version = OS_SYSCALL_ABI_VERSION;
        request.index = index;
        status = taskmgr_syscall_one(OS_SYS_PROCESS_ENUMERATE,
                                     (uint64_t)(uintptr_t)&request);
        if (status == -2) break;
        if (status < 0) {
            g_entry_count = 0U;
            g_selection_valid = false;
            set_status("PROCESS QUERY FAILED");
            return;
        }
        if (index == TASKMGR_MAX_PROCESSES) {
            g_entries_truncated = true;
            break;
        }
        taskmgr_entry_t *entry = &g_entries[g_entry_count++];
        entry->thread = false;
        entry->id = request.info.pid;
        entry->parent_id = request.info.parent_pid;
        entry->state = request.info.state;
        entry->cpu = 0U;
        entry->thread_count = request.info.thread_count;
        copy_text(entry->name, sizeof(entry->name), request.info.name);
    }
    for (uint32_t index = 0U; index <= TASKMGR_MAX_THREADS; ++index) {
        os_thread_enumerate_t request = {0};
        int64_t status;
        request.hdr.size = sizeof(request);
        request.hdr.version = OS_SYSCALL_ABI_VERSION;
        request.index = index;
        status = taskmgr_syscall_one(OS_SYS_THREAD_ENUMERATE,
                                     (uint64_t)(uintptr_t)&request);
        if (status == -2) break;
        if (status < 0) {
            g_entry_count = 0U;
            g_selection_valid = false;
            set_status("THREAD QUERY FAILED");
            return;
        }
        if (index == TASKMGR_MAX_THREADS) {
            g_entries_truncated = true;
            break;
        }
        taskmgr_entry_t *entry = &g_entries[g_entry_count++];
        entry->thread = true;
        entry->id = request.info.tid;
        entry->parent_id = request.info.process_pid;
        entry->state = request.info.state;
        entry->cpu = request.info.current_cpu;
        entry->thread_count = 0U;
        copy_text(entry->name, sizeof(entry->name), request.info.name);
    }
    g_first_row = 0U;
    g_selection_valid = g_entry_count != 0U;
    g_selected_entry = 0U;
    if (selected_id != 0U) {
        for (uint32_t index = 0U; index < g_entry_count; ++index) {
            if (g_entries[index].id == selected_id &&
                g_entries[index].thread == selected_thread) {
                g_selected_entry = index;
                break;
            }
        }
    }
    set_status(g_entries_truncated ? "LIST LIMIT  R REFRESH  CTRL +/- FONT" :
                                    "R REFRESH  CTRL +/- FONT  ESC CLOSE");
}

static void fill_rect(uint32_t x, uint32_t y, uint32_t width,
                      uint32_t height, uint32_t color) {
    if (g_window.pixels == 0 || x >= g_window.width || y >= g_window.height) return;
    if (width > g_window.width - x) width = g_window.width - x;
    if (height > g_window.height - y) height = g_window.height - y;
    for (uint32_t row = 0U; row < height; ++row) {
        uint32_t *line = g_window.pixels +
            (uint64_t)(y + row) * g_window.width + x;
        for (uint32_t column = 0U; column < width; ++column) line[column] = color;
    }
}

static void draw_text(uint32_t x, uint32_t y, const char *text, uint32_t color) {
    liteos_text_draw(g_window.pixels, g_window.width,
                     g_window.width, g_window.height,
                     (int32_t)x, (int32_t)y, text, color);
}

static void draw_number(uint32_t x, uint32_t y, uint64_t value, uint32_t color) {
    char text[24] = {0};
    append_u64(text, sizeof(text), value);
    draw_text(x, y, text, color);
}

static void draw_button(uint32_t x, uint32_t y, uint32_t width,
                        const char *text) {
    user_client_chrome_round_rect(g_window.pixels, g_window.width,
                                  g_window.width, g_window.height,
                                  x, y, width, 30U, TASKMGR_ACCENT);
    draw_text(x + 12U, y + 4U, text, 0x00FFFFFFU);
}

static uint32_t visible_rows(void) {
    uint32_t bottom = g_window.height > TASKMGR_STATUS_HEIGHT ?
                      g_window.height - TASKMGR_STATUS_HEIGHT : 0U;
    return bottom > TASKMGR_TABLE_TOP ?
        (bottom - TASKMGR_TABLE_TOP) / TASKMGR_ROW_HEIGHT : 0U;
}

static void ensure_selection_visible(void) {
    uint32_t rows = visible_rows();
    if (!g_selection_valid || rows == 0U) return;
    if (g_selected_entry < g_first_row) g_first_row = g_selected_entry;
    if (g_selected_entry >= g_first_row + rows) {
        g_first_row = g_selected_entry - rows + 1U;
    }
    if (g_first_row > g_entry_count) g_first_row = g_entry_count;
}

static void select_delta(int32_t delta) {
    int64_t next;
    if (g_entry_count == 0U) return;
    if (!g_selection_valid) {
        g_selection_valid = true;
        g_selected_entry = 0U;
        ensure_selection_visible();
        return;
    }
    next = (int64_t)g_selected_entry + delta;
    if (next < 0) next = 0;
    if (next >= (int64_t)g_entry_count) next = (int64_t)g_entry_count - 1;
    g_selected_entry = (uint32_t)next;
    ensure_selection_visible();
}

static void draw_row(uint32_t row, uint32_t y) {
    const taskmgr_entry_t *entry = &g_entries[row];
    bool running = entry->thread ? entry->state == OS_THREAD_RUNNING :
                                   entry->state == OS_PROCESS_RUNNING;
    uint32_t color = row == g_selected_entry && g_selection_valid ?
                     TASKMGR_SELECTED : 0x00FFFFFFU;
    fill_rect(0U, y, g_window.width, TASKMGR_ROW_HEIGHT, color);
    draw_text(16U, y + 5U, entry->thread ? "THREAD" : "PROCESS", TASKMGR_MUTED);
    draw_number(112U, y + 5U, entry->id, TASKMGR_TEXT);
    draw_number(214U, y + 5U, entry->parent_id, TASKMGR_MUTED);
    draw_text(316U, y + 5U, entry->name, TASKMGR_TEXT);
    draw_text(650U, y + 5U,
               entry->thread ? thread_state_text(entry->state) :
                                process_state_text(entry->state),
               running ? 0x002A7A43U : TASKMGR_MUTED);
    if (entry->thread) draw_number(790U, y + 5U, entry->cpu, TASKMGR_TEXT);
    else draw_text(790U, y + 5U, "-", TASKMGR_MUTED);
    if (entry->thread) draw_text(870U, y + 5U, "-", TASKMGR_MUTED);
    else draw_number(870U, y + 5U, entry->thread_count, TASKMGR_TEXT);
}

static void render(void) {
    uint32_t rows = visible_rows();
    fill_rect(0U, 0U, g_window.width, g_window.height, TASKMGR_BACKGROUND);
    fill_rect(0U, 0U, g_window.width, USER_CLIENT_CHROME_HEIGHT, TASKMGR_HEADER);
    user_client_chrome_app_icon(g_window.pixels, g_window.width,
                                g_window.width, g_window.height,
                                16U, USER_CLIENT_CHROME_TITLE_Y,
                                USER_CLIENT_CHROME_ICON_TASKS);
    draw_text(50U, USER_CLIENT_CHROME_TITLE_Y, "TASK MANAGER", TASKMGR_TEXT);
    draw_button(20U, 57U, 112U, "REFRESH");
    draw_text(154U, 63U, "R / F5", TASKMGR_MUTED);

    fill_rect(0U, 98U, g_window.width, 18U, TASKMGR_TABLE_HEADER);
    draw_text(16U, 98U, "TYPE", TASKMGR_MUTED);
    draw_text(112U, 98U, "ID", TASKMGR_MUTED);
    draw_text(214U, 98U, "PARENT/PID", TASKMGR_MUTED);
    draw_text(316U, 98U, "NAME", TASKMGR_MUTED);
    draw_text(650U, 98U, "STATE", TASKMGR_MUTED);
    draw_text(790U, 98U, "CPU", TASKMGR_MUTED);
    draw_text(870U, 98U, "THREADS", TASKMGR_MUTED);

    for (uint32_t visible = 0U; visible < rows; ++visible) {
        uint32_t row = g_first_row + visible;
        if (row >= g_entry_count) break;
        draw_row(row, TASKMGR_TABLE_TOP + visible * TASKMGR_ROW_HEIGHT);
    }
    fill_rect(0U, g_window.height - TASKMGR_STATUS_HEIGHT,
              g_window.width, TASKMGR_STATUS_HEIGHT, TASKMGR_TABLE_HEADER);
    draw_text(16U, g_window.height - 27U, g_status, TASKMGR_TEXT);
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
    (void)taskmgr_syscall_one(OS_SYS_WINDOW_UPDATE,
                              (uint64_t)(uintptr_t)&update);
}

static bool create_window(void) {
    os_display_info_t display = {0};
    os_window_create_t request = {0};
    uint32_t width;
    uint32_t height;
    display.hdr.size = sizeof(display);
    display.hdr.version = OS_SYSCALL_ABI_VERSION;
    if (taskmgr_syscall_one(OS_SYS_DISPLAY_GET_INFO,
                            (uint64_t)(uintptr_t)&display) < 0 ||
        display.width < 760U || display.height < 420U) return false;
    width = display.width * 4U / 5U;
    height = display.height * 3U / 4U;
    if (width > 1040U) width = 1040U;
    if (height > 680U) height = 680U;
    if (width < 760U) width = 760U;
    if (height < 420U) height = 420U;
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.x = (int32_t)((display.width - width) / 2U);
    request.y = (int32_t)((display.height - height) / 2U);
    request.width = width;
    request.height = height;
    request.flags = OS_WINDOW_VISIBLE | OS_WINDOW_RESIZABLE |
                    OS_WINDOW_CLIENT_DECORATIONS;
    request.background = TASKMGR_BACKGROUND;
    request.title[0] = 'T'; request.title[1] = 'A'; request.title[2] = 'S';
    request.title[3] = 'K'; request.title[4] = ' '; request.title[5] = 'M';
    request.title[6] = 'A'; request.title[7] = 'N'; request.title[8] = 'A';
    request.title[9] = 'G'; request.title[10] = 'E'; request.title[11] = 'R';
    request.address = TASKMGR_MAP_BASE;
    if (taskmgr_syscall_one(OS_SYS_WINDOW_CREATE,
                            (uint64_t)(uintptr_t)&request) < 0 ||
        request.window == OS_INVALID_HANDLE || request.address == 0U) return false;
    g_window.handle = request.window;
    g_window.identifier = request.identifier;
    g_window.width = request.width;
    g_window.height = request.height;
    g_window.pixels = (uint32_t *)(uintptr_t)request.address;
    return true;
}

static bool key_is(const os_input_event_t *input, uint32_t hid, char ascii) {
    return input != 0 && (input->code == hid || input->code == (uint32_t)ascii);
}

static void handle_event(const os_window_event_t *event) {
    const os_input_event_t *input;
    if (event == 0) return;
    if (event->type == OS_WINDOW_EVENT_CLOSE_REQUEST) taskmgr_exit(0U);
    if (event->type == OS_WINDOW_EVENT_RESIZE) {
        if (event->resize.width == 0U || event->resize.height == 0U ||
            (uint64_t)event->resize.width * event->resize.height * 4U >
                event->resize.buffer_size) return;
        g_window.width = event->resize.width;
        g_window.height = event->resize.height;
        ensure_selection_visible();
        render();
        return;
    }
    if (event->type != OS_WINDOW_EVENT_INPUT) return;
    input = &event->input;
    if (input->type == OS_INPUT_EVENT_KEY &&
        (input->code == 0xE0U || input->code == 0xE4U)) {
        g_ctrl = input->value != OS_INPUT_VALUE_RELEASE;
        return;
    }
    if (input->type == OS_INPUT_EVENT_BUTTON &&
        input->code == OS_INPUT_BUTTON_LEFT &&
        input->value == OS_INPUT_VALUE_PRESS) {
        if (user_client_chrome_close_hit(event->pointer_x, event->pointer_y,
                                         g_window.width,
                                         USER_CLIENT_CHROME_HEIGHT)) {
            taskmgr_exit(0U);
        }
        if (event->pointer_y >= 57 && event->pointer_y < 87 &&
            event->pointer_x >= 20 && event->pointer_x < 132) {
            refresh_processes();
            render();
            return;
        }
        uint32_t rows = visible_rows();
        if (event->pointer_y >= (int32_t)TASKMGR_TABLE_TOP &&
            event->pointer_y < (int32_t)(TASKMGR_TABLE_TOP + rows * TASKMGR_ROW_HEIGHT)) {
            uint32_t row = g_first_row +
                ((uint32_t)event->pointer_y - TASKMGR_TABLE_TOP) / TASKMGR_ROW_HEIGHT;
            if (row < g_entry_count) {
                g_selected_entry = row;
                g_selection_valid = true;
                render();
            }
        }
        return;
    }
    if (input->type == OS_INPUT_EVENT_RELATIVE &&
        input->code == OS_INPUT_REL_WHEEL && input->value != 0) {
        if (g_ctrl) {
            (void)liteos_text_adjust(input->value > 0 ? 1 : -1);
            render();
            return;
        }
        select_delta(input->value > 0 ? -3 : 3);
        render();
        return;
    }
    if (input->type != OS_INPUT_EVENT_KEY || input->value == OS_INPUT_VALUE_RELEASE) return;
    if (key_is(input, 0x29U, 27) || key_is(input, 0x14U, 'q')) taskmgr_exit(0U);
    if (g_ctrl && input->code == 0x2EU) {
        (void)liteos_text_adjust(1);
    } else if (g_ctrl && input->code == 0x2DU) {
        (void)liteos_text_adjust(-1);
    } else if (key_is(input, 0x15U, 'r') || input->code == 0x3EU) {
        refresh_processes();
    } else if (input->code == 0x52U) {
        select_delta(-1);
    } else if (input->code == 0x51U) {
        select_delta(1);
    } else if (input->code == 0x4BU) {
        select_delta(-(int32_t)visible_rows());
    } else if (input->code == 0x4EU) {
        select_delta((int32_t)visible_rows());
    } else {
        return;
    }
    render();
}

int main(void) {
    if (!liteos_text_init(LITEOS_TEXT_DEFAULT_SIZE) ||
        !create_window()) return 1;
    refresh_processes();
    render();
    for (;;) {
        os_window_event_read_t request = {0};
        request.hdr.size = sizeof(request);
        request.hdr.version = OS_SYSCALL_ABI_VERSION;
        request.identifier = g_window.identifier;
        request.timeout_ns = TASKMGR_EVENT_WAIT;
        int64_t status = taskmgr_syscall_one(OS_SYS_WINDOW_EVENT_READ,
                                             (uint64_t)(uintptr_t)&request);
        if (status == 0) handle_event(&request.event);
        else if (status != -11 && status != -110) __asm__ volatile ("pause");
    }
}

__attribute__((noreturn)) void taskmgr_entry(void) {
    taskmgr_exit((uint64_t)(main() == 0 ? 0 : 1));
}
