#include <stdbool.h>
#include <stdint.h>

#include <uapi/all.h>

#include <uapi/window_theme.h>
#include "../../runtime/caption.h"
#include "../../runtime/paint.h"
#include "../../runtime/text.h"

#define TASKMGR_MAP_BASE       0x0E000000ULL
#define TASKMGR_REFRESH_NS 1000000000ULL
#define TASKMGR_MAX_PROCESSES   256U
#define TASKMGR_MAX_THREADS     512U
#define TASKMGR_MAX_ENTRIES     (TASKMGR_MAX_PROCESSES + TASKMGR_MAX_THREADS)
#define TASKMGR_ROW_HEIGHT      (ui_text_line_height() + 14U)
#define TASKMGR_BUTTON_HEIGHT   (ui_text_line_height() + 12U)
#define TASKMGR_TABLE_TOP       TASKMGR_ROW_HEIGHT
#define TASKMGR_STATUS_HEIGHT   (ui_text_line_height() + 16U)
#define TASKMGR_STATUS_CAP      96U

#define TASKMGR_BACKGROUND      OS_THEME_SURFACE
#define TASKMGR_HEADER          OS_THEME_HEADER
#define TASKMGR_TABLE_HEADER    OS_THEME_TOOLBAR
#define TASKMGR_TEXT            OS_THEME_TEXT
#define TASKMGR_MUTED           OS_THEME_MUTED
#define TASKMGR_ACCENT          OS_THEME_ACCENT
#define TASKMGR_SELECTED        OS_THEME_SELECTED

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
    bool expanded;
    bool cpu_sample_valid;
    os_task_info_t resources;
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
static taskmgr_entry_t g_previous[TASKMGR_MAX_ENTRIES];
static uint32_t g_previous_count;
static uint64_t g_sample_time;
static uint64_t g_next_refresh;
static uint64_t g_confirm_pid;
static uint64_t g_confirm_created;
static uint64_t g_confirm_tid;
static bool g_menu_open;
static uint32_t g_menu_x, g_menu_y;
static int32_t g_menu_hover = -1;
static void dismiss_menu(void);
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

static uint64_t monotonic_ns(void) {
    os_timespec_t time = {0};
    if (taskmgr_syscall6(OS_SYS_CLOCK_GET, OS_CLOCK_MONOTONIC,
            (uint64_t)(uintptr_t)&time, 0U, 0U, 0U, 0U) < 0) return 0U;
    return (uint64_t)time.seconds * 1000000000ULL + (uint64_t)time.nanoseconds;
}

static void sample_resources(taskmgr_entry_t *entry, uint64_t now) {
    entry->resources = (os_task_info_t){0};
    entry->resources.hdr.size = sizeof(entry->resources);
    entry->resources.hdr.version = OS_SYSCALL_ABI_VERSION;
    entry->resources.pid = entry->thread ? entry->parent_id : entry->id;
    entry->resources.tid = entry->thread ? entry->id : 0U;
    entry->cpu = 0U;
    entry->cpu_sample_valid = false;
    entry->expanded = false;
    if (taskmgr_syscall_one(OS_SYS_TASK_QUERY,
            (uint64_t)(uintptr_t)&entry->resources) < 0) {
        entry->resources.flags = OS_TASK_PROTECTED;
        return;
    }
    for (uint32_t i = 0; i < g_previous_count; ++i) {
        const taskmgr_entry_t *old = &g_previous[i];
        if (old->id != entry->id || old->thread != entry->thread ||
            old->parent_id != entry->parent_id ||
            old->resources.create_time_ns != entry->resources.create_time_ns) continue;
        entry->expanded = old->expanded;
        if (now > g_sample_time && entry->resources.runtime_ns >= old->resources.runtime_ns) {
            uint64_t elapsed = now - g_sample_time;
            uint32_t cpus = entry->resources.cpu_count;
            if (!cpus) cpus = 1U;
            uint64_t delta = entry->resources.runtime_ns - old->resources.runtime_ns;
            /* Three fractional digits keep low but real CPU usage visible. */
            uint64_t thousandths = delta <= UINT64_MAX / 100000U ?
                delta * 100000U / elapsed / cpus : delta / elapsed * 100000U / cpus;
            entry->cpu = thousandths > 100000U ? 100000U : (uint32_t)thousandths;
            entry->cpu_sample_valid = true;
        }
        break;
    }
}

static bool entry_visible(uint32_t index) {
    if (!g_entries[index].thread) return true;
    for (uint32_t i = 0; i < g_entry_count; ++i)
        if (!g_entries[i].thread && g_entries[i].id == g_entries[index].parent_id)
            return g_entries[i].expanded;
    return false;
}

static uint32_t visible_entry(uint32_t row) {
    /* Process enumeration order is stable; threads live directly under their process. */
    for (uint32_t i = 0; i < g_entry_count; ++i) {
        if (g_entries[i].thread) continue;
        if (row-- == 0U) return i;
        if (!g_entries[i].expanded) continue;
        for (uint32_t j = 0; j < g_entry_count; ++j)
            if (g_entries[j].thread && g_entries[j].parent_id == g_entries[i].id)
                if (row-- == 0U) return j;
    }
    return g_entry_count;
}

static uint32_t entry_row(uint32_t index) {
    for (uint32_t row = 0; row < g_entry_count; ++row)
        if (visible_entry(row) == index) return row;
    return 0U;
}

static uint32_t visible_count(void) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < g_entry_count; ++i) if (entry_visible(i)) ++count;
    return count;
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
    uint64_t selected_created = g_selection_valid && g_selected_entry < g_entry_count ?
        g_entries[g_selected_entry].resources.create_time_ns : 0U;
    g_previous_count = g_entry_count;
    for (uint32_t i = 0; i < g_entry_count; ++i) g_previous[i] = g_entries[i];
    uint64_t now = monotonic_ns();
    g_next_refresh = now + TASKMGR_REFRESH_NS;
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
        sample_resources(entry, now);
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
        sample_resources(entry, now);
    }
    g_sample_time = now;
    g_next_refresh = now + TASKMGR_REFRESH_NS;
    g_selection_valid = g_entry_count != 0U;
    g_selected_entry = 0U;
    bool restored = false;
    if (selected_id != 0U) {
        for (uint32_t index = 0U; index < g_entry_count; ++index) {
            if (g_entries[index].id == selected_id &&
                g_entries[index].thread == selected_thread &&
                g_entries[index].resources.create_time_ns == selected_created &&
                entry_visible(index)) {
                g_selected_entry = index;
                restored = true;
                break;
            }
        }
    }
    if (selected_id != 0U && !restored) dismiss_menu();
    uint32_t count = visible_count();
    if (g_first_row >= count) g_first_row = count ? count - 1U : 0U;
    if (!g_confirm_pid) set_status(g_entries_truncated ? "LIST LIMIT  AUTO REFRESH 1s" :
        "AUTO REFRESH 1s  |  CPU: MACHINE %  |  MEMORY: RESIDENT MiB");
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
    uint32_t right = g_window.width > 12U ? g_window.width - 12U : 0U;
    if (x == g_window.width * 36U / 100U) right = g_window.width * 48U / 100U - 12U;
    else if (x == g_window.width * 48U / 100U) right = g_window.width * 67U / 100U - 12U;
    else if (x == g_window.width * 67U / 100U) right = g_window.width * 81U / 100U - 12U;
    ui_text_draw_clipped(g_window.pixels, g_window.width,
                     g_window.width, g_window.height,
                     (int32_t)x, (int32_t)y, right, text, color);
}

static void draw_number(uint32_t x, uint32_t y, uint64_t value, uint32_t color) {
    char text[24] = {0};
    append_u64(text, sizeof(text), value);
    draw_text(x, y, text, color);
}

static uint32_t menu_width(void) {
    uint32_t width = ui_text_measure("Confirm end process") + 32U;
    return width < g_window.width ? width : g_window.width;
}

static uint32_t menu_rows(void) {
    return g_selected_entry < g_entry_count && g_entries[g_selected_entry].thread ? 2U : 1U;
}

static int32_t menu_hit(int32_t x, int32_t y) {
    if (!g_menu_open || x < (int32_t)g_menu_x || y < (int32_t)g_menu_y ||
        (uint32_t)x - g_menu_x >= menu_width() ||
        (uint32_t)y - g_menu_y >= menu_rows() * TASKMGR_ROW_HEIGHT) return -1;
    return ((uint32_t)y - g_menu_y) / TASKMGR_ROW_HEIGHT;
}

static void dismiss_menu(void) {
    g_menu_open = false;
    g_menu_hover = -1;
    g_confirm_pid = 0U;
    g_confirm_tid = 0U;
}

static void draw_context_menu(void) {
    if (!g_menu_open || g_selected_entry >= g_entry_count) return;
    uint32_t width = menu_width();
    uint32_t height = menu_rows() * TASKMGR_ROW_HEIGHT;
    if (g_menu_x > g_window.width - width) g_menu_x = g_window.width - width;
    if (height > g_window.height) height = g_window.height;
    if (g_menu_y > g_window.height - height) g_menu_y = g_window.height - height;
    user_paint_round_rect(g_window.pixels, g_window.width,
        g_window.width, g_window.height, g_menu_x, g_menu_y, width, height, OS_THEME_BORDER);
    for (uint32_t row = 0; row < menu_rows(); ++row) {
        bool thread_only = g_entries[g_selected_entry].thread && row == 0U;
        bool confirm = g_confirm_pid != 0U && ((g_confirm_tid != 0U) == thread_only);
        uint32_t y = g_menu_y + row * TASKMGR_ROW_HEIGHT;
        fill_rect(g_menu_x + 1U, y + 1U, width - 2U, TASKMGR_ROW_HEIGHT - 2U,
            g_menu_hover == (int32_t)row ? OS_THEME_SELECTED : OS_THEME_SURFACE);
        ui_text_draw_clipped(g_window.pixels, g_window.width, g_window.width,
            g_window.height, (int32_t)g_menu_x + 12, (int32_t)y + 6,
            g_menu_x + width - 12U,
            thread_only ? (confirm ? "Confirm end thread" : "End thread") :
                (confirm ? "Confirm end process" : "End process"),
            g_entries[g_selected_entry].resources.flags & OS_TASK_PROTECTED ? TASKMGR_MUTED : 0x00B3261EU);
    }
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
    uint32_t selected_row = entry_row(g_selected_entry);
    if (selected_row < g_first_row) g_first_row = selected_row;
    if (selected_row >= g_first_row + rows) {
        g_first_row = selected_row - rows + 1U;
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
    next = (int64_t)entry_row(g_selected_entry) + delta;
    if (next < 0) next = 0;
    if (next >= (int64_t)visible_count()) next = (int64_t)visible_count() - 1;
    g_selected_entry = visible_entry((uint32_t)next);
    dismiss_menu();
    ensure_selection_visible();
}

static void draw_row(uint32_t row, uint32_t y) {
    const taskmgr_entry_t *entry = &g_entries[row];
    bool running = entry->thread ? entry->state == OS_THREAD_RUNNING :
                                   entry->state == OS_PROCESS_RUNNING;
    uint32_t color = row == g_selected_entry && g_selection_valid ?
                     TASKMGR_SELECTED : (entry_row(row) & 1U ? TASKMGR_BACKGROUND : 0x00FFFFFFU);
    fill_rect(0U, y, g_window.width, TASKMGR_ROW_HEIGHT, color);
    draw_text(16U, y + 6U, entry->thread ? "" : (entry->expanded ? "v" : ">"), TASKMGR_MUTED);
    ui_text_draw_rect(g_window.pixels, g_window.width, g_window.width, g_window.height,
        entry->thread ? 64 : 44, (int32_t)y + 6, 0, y,
        g_window.width * 36U / 100U - 12U, y + TASKMGR_ROW_HEIGHT,
        entry->name, TASKMGR_TEXT);
    draw_number(g_window.width * 36U / 100U, y + 6U, entry->id, TASKMGR_MUTED);
    draw_text(g_window.width * 48U / 100U, y + 6U,
               entry->thread ? thread_state_text(entry->state) :
                                process_state_text(entry->state),
               running ? 0x002A7A43U : TASKMGR_MUTED);
    char value[32] = {0};
    append_u64(value, sizeof(value), entry->cpu / 1000U);
    append_text(value, sizeof(value), ".");
    uint32_t fractional = entry->cpu % 1000U;
    if (fractional < 100U) append_text(value, sizeof(value), "0");
    if (fractional < 10U) append_text(value, sizeof(value), "0");
    append_u64(value, sizeof(value), fractional);
    append_text(value, sizeof(value), "%");
    draw_text(g_window.width * 67U / 100U, y + 6U,
        entry->cpu_sample_valid ? value : "-", TASKMGR_TEXT);
    if (entry->thread) draw_text(g_window.width * 81U / 100U, y + 6U, "-", TASKMGR_MUTED);
    else {
        value[0] = '\0';
        uint64_t kib = entry->resources.rss_bytes / 1024U;
        append_u64(value, sizeof(value), kib / 1024U);
        append_text(value, sizeof(value), ".");
        append_u64(value, sizeof(value), (kib % 1024U) * 10U / 1024U);
        draw_text(g_window.width * 81U / 100U, y + 6U, value, TASKMGR_TEXT);
    }
}

static void render(void) {
    uint32_t rows = visible_rows();
    fill_rect(0U, 0U, g_window.width, g_window.height, TASKMGR_BACKGROUND);

    uint32_t header_y = TASKMGR_TABLE_TOP - TASKMGR_ROW_HEIGHT;
    fill_rect(0U, header_y, g_window.width, TASKMGR_ROW_HEIGHT, TASKMGR_TABLE_HEADER);
    draw_text(44U, header_y + 6U, "Name", TASKMGR_MUTED);
    draw_text(g_window.width * 36U / 100U, header_y + 6U, "ID", TASKMGR_MUTED);
    draw_text(g_window.width * 48U / 100U, header_y + 6U, "State", TASKMGR_MUTED);
    draw_text(g_window.width * 67U / 100U, header_y + 6U, "CPU", TASKMGR_MUTED);
    draw_text(g_window.width * 81U / 100U, header_y + 6U, "MiB", TASKMGR_MUTED);

    for (uint32_t visible = 0U; visible < rows; ++visible) {
        uint32_t row = visible_entry(g_first_row + visible);
        if (row >= g_entry_count) break;
        draw_row(row, TASKMGR_TABLE_TOP + visible * TASKMGR_ROW_HEIGHT);
    }
    uint32_t status_top = g_window.height > TASKMGR_STATUS_HEIGHT ?
        g_window.height - TASKMGR_STATUS_HEIGHT : 0U;
    fill_rect(0U, status_top,
              g_window.width, TASKMGR_STATUS_HEIGHT, TASKMGR_TABLE_HEADER);
    ui_text_draw_clipped(g_window.pixels, g_window.width, g_window.width,
        g_window.height, 16, (int32_t)status_top + 8,
        g_window.width > 16U ? g_window.width - 16U : 0U, g_status, TASKMGR_MUTED);
    draw_context_menu();

    os_window_update_t update = {0};
    update.hdr.size = sizeof(update);
    update.hdr.version = OS_SYSCALL_ABI_VERSION;
    update.identifier = g_window.identifier;
    update.flags = OS_WINDOW_CAPTION_HEIGHT(user_caption_height());
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
                    OS_WINDOW_STANDARD_CAPTION_CONTROLS;
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

static void end_selected_process(bool thread_only) {
    if (!g_selection_valid || g_selected_entry >= g_entry_count) return;
    taskmgr_entry_t *entry = &g_entries[g_selected_entry];
    uint64_t pid = entry->thread ? entry->parent_id : entry->id;
    uint64_t tid = thread_only && entry->thread ? entry->id : 0U;
    if (entry->resources.flags & OS_TASK_PROTECTED) {
        g_confirm_pid = 0U;
        set_status("SYSTEM PROCESS: CANNOT END");
        return;
    }
    if (g_confirm_pid != pid || g_confirm_tid != tid || g_confirm_created != entry->resources.create_time_ns) {
        g_confirm_pid = pid;
        g_confirm_tid = tid;
        g_confirm_created = entry->resources.create_time_ns;
        set_status(thread_only ? "Click Confirm end thread to terminate only this thread" :
            "Click Confirm end process to terminate the whole process");
        return;
    }
    os_process_terminate_t request = {0};
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.pid = pid;
    request.create_time_ns = g_confirm_created;
    int64_t status;
    if (tid != 0U) {
        os_thread_terminate_t thread = {0};
        thread.hdr = request.hdr;
        thread.hdr.size = sizeof(thread);
        thread.pid = pid; thread.tid = tid; thread.create_time_ns = g_confirm_created;
        status = taskmgr_syscall_one(OS_SYS_THREAD_TERMINATE, (uint64_t)(uintptr_t)&thread);
    } else status = taskmgr_syscall_one(OS_SYS_PROCESS_TERMINATE, (uint64_t)(uintptr_t)&request);
    dismiss_menu();
    refresh_processes();
    set_status(status == 0 ? "Termination requested" : "Cannot end this process");
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
        dismiss_menu();
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
    if (input->type == OS_INPUT_EVENT_RELATIVE &&
        (input->code == OS_INPUT_REL_X || input->code == OS_INPUT_REL_Y) && g_menu_open) {
        int32_t hover = menu_hit(event->pointer_x, event->pointer_y);
        if (hover != g_menu_hover) { g_menu_hover = hover; render(); }
        return;
    }
    if (input->type == OS_INPUT_EVENT_BUTTON &&
        (input->code == OS_INPUT_BUTTON_LEFT || input->code == OS_INPUT_BUTTON_RIGHT) &&
        input->value == OS_INPUT_VALUE_PRESS) {
        if (g_menu_open && input->code == OS_INPUT_BUTTON_LEFT) {
            int32_t row = menu_hit(event->pointer_x, event->pointer_y);
            if (row >= 0) {
                end_selected_process(g_entries[g_selected_entry].thread && row == 0);
                render();
                return;
            }
            dismiss_menu();
            render();
        }
        uint32_t rows = visible_rows();
        if (event->pointer_y >= (int32_t)TASKMGR_TABLE_TOP &&
            event->pointer_y < (int32_t)(TASKMGR_TABLE_TOP + rows * TASKMGR_ROW_HEIGHT)) {
            uint32_t row = visible_entry(g_first_row +
                ((uint32_t)event->pointer_y - TASKMGR_TABLE_TOP) / TASKMGR_ROW_HEIGHT);
            if (row < g_entry_count) {
                g_selected_entry = row;
                g_selection_valid = true;
                dismiss_menu();
                if (input->code == OS_INPUT_BUTTON_RIGHT) {
                    g_menu_open = true;
                    g_menu_x = event->pointer_x > 0 ? (uint32_t)event->pointer_x : 0U;
                    g_menu_y = event->pointer_y > 0 ? (uint32_t)event->pointer_y : 0U;
                } else if (!g_entries[row].thread && event->pointer_x >= 0 && event->pointer_x < 44)
                    g_entries[row].expanded = !g_entries[row].expanded;
                render();
            }
        }
        return;
    }
    if (input->type == OS_INPUT_EVENT_RELATIVE &&
        input->code == OS_INPUT_REL_WHEEL && input->value != 0) {
        if (g_ctrl) {
            (void)ui_text_adjust(input->value > 0 ? 1 : -1);
            render();
            return;
        }
        select_delta(input->value > 0 ? -3 : 3);
        render();
        return;
    }
    if (input->type != OS_INPUT_EVENT_KEY || input->value == OS_INPUT_VALUE_RELEASE) return;
    if (key_is(input, 0x29U, 27) && g_menu_open) { dismiss_menu(); render(); return; }
    if (key_is(input, 0x29U, 27) || key_is(input, 0x14U, 'q')) taskmgr_exit(0U);
    if (g_ctrl && input->code == 0x2EU) {
        (void)ui_text_adjust(1);
    } else if (g_ctrl && input->code == 0x2DU) {
        (void)ui_text_adjust(-1);
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
    if (!ui_text_init(OS_TEXT_DEFAULT_SIZE) ||
        !create_window()) return 1;
    refresh_processes();
    render();
    for (;;) {
        os_window_event_read_t request = {0};
        request.hdr.size = sizeof(request);
        request.hdr.version = OS_SYSCALL_ABI_VERSION;
        request.identifier = g_window.identifier;
        uint64_t now = monotonic_ns();
        if (now >= g_next_refresh) {
            refresh_processes();
            render();
            now = monotonic_ns();
        }
        request.timeout_ns = now < g_next_refresh ? g_next_refresh - now : 0U;
        int64_t status = taskmgr_syscall_one(OS_SYS_WINDOW_EVENT_READ,
                                             (uint64_t)(uintptr_t)&request);
        if (status == 0) handle_event(&request.event);
        else if (status != -11 && status != -110) __asm__ volatile ("pause");
    }
}

__attribute__((noreturn)) void taskmgr_entry(void) {
    taskmgr_exit((uint64_t)(main() == 0 ? 0 : 1));
}
