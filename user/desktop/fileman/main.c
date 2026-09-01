#include <stdint.h>
#include <stdbool.h>

#if LITEOS_REALTEST
#include <stdio.h>
#endif

#include <uapi/all.h>

#include "../../runtime/liteos_text.h"

#define FILEMAN_MAP_BASE       0x0A000000ULL
#define FILEMAN_EVENT_WAIT OS_WAIT_INFINITE
#define FILEMAN_PATH_CAPACITY  256U
#define FILEMAN_ENTRY_CAPACITY 64U

/*
 * GNOME-Files-like client UI.
 */
#define FM4_SIDEBAR_WIDTH          198U
#define FM4_HEADER_HEIGHT           40U
#define FM4_HEADER_CONTROL_Y        3U
#define FM4_HEADER_ICON_Y           12U
#define FM4_HEADER_CLOSE_Y          8U
#define FM4_HEADER_MENU_Y           13U
/* Blend2D's baseline puts the title strokes on the same centerline as the
 * 16px header icons. */
#define FM4_HEADER_TEXT_Y           9U
#define FM4_WINDOW_BORDER_COLOR     0x00D2D2D5U

#define FM4_GRID_MARGIN_X           24U
#define FM4_GRID_MARGIN_Y           20U

#define FM4_GRID_CARD_WIDTH        126U
#define FM4_GRID_CARD_HEIGHT       136U
#define FM4_GRID_STEP_X            148U
#define FM4_GRID_STEP_Y            150U

#define FM4_CONTEXT_WIDTH          180U
#define FM4_CONTEXT_ROW_HEIGHT      34U

/* The sidebar is intentionally a volume shelf.  Navigation shortcuts such
 * as Home/Recent/Trash consume valuable space and do not describe storage
 * devices, which is the useful information in a small LiteOS desktop. */
#define FM4_VOLUME_CAPACITY         16U
#define FM4_VOLUME_ROW_HEIGHT       39U
#define FM4_VOLUME_FIRST_Y          50U

#define FM4_HISTORY_CAPACITY        16U
/*
 * input.timestamp currently contains raw x86 TSC ticks.
 *
 * Do not label this value as nanoseconds. 3 billion TSC ticks gives a
 * practical desktop double-click window on the current high-frequency
 * x86_64 environment.
 */
#define FM4_DOUBLE_CLICK_TSC_TICKS 3000000000ULL

#define FM4_INDEX_NONE      0xFFFFFFFFU


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

typedef struct fileman_volume {
    char label[OS_FILE_NAME_MAX];
    char path[FILEMAN_PATH_CAPACITY];
} fileman_volume_t;

static const char g_notepad_path[] = "/sbin/notepad";
static const char g_notepad_name[] = "notepad";
static fileman_window_t g_window = {OS_INVALID_HANDLE, 0U, 0U, 0U, 0};
static fileman_entry_t g_entries[FILEMAN_ENTRY_CAPACITY];
static fileman_volume_t g_volumes[FM4_VOLUME_CAPACITY];
static char g_path[FILEMAN_PATH_CAPACITY] = "/";
static char g_status[128] = "READY";
static uint32_t g_entry_count;
static uint32_t g_volume_count;
static uint32_t g_selected;
static uint32_t g_first_entry;
static bool g_ctrl;
static bool g_shift;

static bool g_selection_valid;

static uint32_t g_hover_item =
    FM4_INDEX_NONE;

static uint32_t g_hover_control;

static int32_t g_pointer_x;
static int32_t g_pointer_y;

static uint32_t g_last_click_item =
    FM4_INDEX_NONE;

static uint64_t g_last_click_time;

static bool g_context_visible;
static uint32_t g_context_x;
static uint32_t g_context_y;

static bool g_delete_dialog;

static char g_history[
    FM4_HISTORY_CAPACITY]
    [FILEMAN_PATH_CAPACITY];

static uint32_t g_history_count;
static uint32_t g_history_index;

static uint32_t *g_target;
static uint32_t g_target_width;
static uint32_t g_target_height;

#define FM4_DAMAGE_CAPACITY 24U

typedef struct fm4_damage_rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} fm4_damage_rect_t;

static fm4_damage_rect_t g_damage[FM4_DAMAGE_CAPACITY];
static uint32_t g_damage_count;
static bool g_damage_full;

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
    register uint64_t r10 __asm__("r10") = 0U;
    __asm__ volatile ("syscall" : "+a"(rax), "+D"(rdi), "+S"(rsi),
                      "+d"(rdx), "+r"(r10) : : "rcx", "r11", "memory");
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

static void fm4_damage_all(void) {
    g_damage_count = 0U;
    g_damage_full = true;
}

static void fm4_damage_reset(void) {
    g_damage_count = 0U;
    g_damage_full = false;
}

static bool fm4_damage_contains(uint32_t x, uint32_t y) {
    if (g_damage_full) return true;
    for (uint32_t index = 0U; index < g_damage_count; ++index) {
        const fm4_damage_rect_t *rect = &g_damage[index];
        if (x >= rect->x && y >= rect->y &&
            x - rect->x < rect->width && y - rect->y < rect->height) {
            return true;
        }
    }
    return false;
}

static bool fm4_damage_touches(const fm4_damage_rect_t *a,
                               const fm4_damage_rect_t *b) {
    uint32_t a_right = a->x + a->width;
    uint32_t a_bottom = a->y + a->height;
    uint32_t b_right = b->x + b->width;
    uint32_t b_bottom = b->y + b->height;

    return a->x <= b_right && b->x <= a_right &&
           a->y <= b_bottom && b->y <= a_bottom;
}

static void fm4_damage_rect(uint32_t x, uint32_t y,
                            uint32_t width, uint32_t height) {
    uint32_t surface_width = g_window.width != 0U ?
                             g_window.width : g_target_width;
    uint32_t surface_height = g_window.height != 0U ?
                              g_window.height : g_target_height;
    fm4_damage_rect_t candidate;

    if (g_damage_full || width == 0U || height == 0U ||
        surface_width == 0U || surface_height == 0U ||
        x >= surface_width || y >= surface_height) return;
    if (width > surface_width - x) width = surface_width - x;
    if (height > surface_height - y) height = surface_height - y;
    candidate.x = x;
    candidate.y = y;
    candidate.width = width;
    candidate.height = height;

    if (g_damage_count >= FM4_DAMAGE_CAPACITY) {
        fm4_damage_all();
        return;
    }
    g_damage[g_damage_count++] = candidate;

    /* Coalesce overlapping or adjacent regions until no pair can merge. */
    for (;;) {
        bool merged = false;
        for (uint32_t first = 0U; first < g_damage_count; ++first) {
            for (uint32_t second = first + 1U;
                 second < g_damage_count;
                 ++second) {
                fm4_damage_rect_t *a = &g_damage[first];
                fm4_damage_rect_t *b = &g_damage[second];
                if (!fm4_damage_touches(a, b)) continue;
                {
                    uint32_t right = a->x + a->width;
                    uint32_t bottom = a->y + a->height;
                    uint32_t other_right = b->x + b->width;
                    uint32_t other_bottom = b->y + b->height;
                    if (b->x < a->x) a->x = b->x;
                    if (b->y < a->y) a->y = b->y;
                    if (other_right > right) right = other_right;
                    if (other_bottom > bottom) bottom = other_bottom;
                    a->width = right - a->x;
                    a->height = bottom - a->y;
                }
                g_damage[second] = g_damage[g_damage_count - 1U];
                --g_damage_count;
                merged = true;
                break;
            }
            if (merged) break;
        }
        if (!merged) break;
    }
}

static void fill_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                      uint32_t color) {
    uint32_t right;
    uint32_t bottom;

    if (g_target == 0 || x >= g_target_width || y >= g_target_height) return;
    if (width > g_target_width - x) width = g_target_width - x;
    if (height > g_target_height - y) height = g_target_height - y;

    right = x + width;
    bottom = y + height;
    if (g_damage_full) {
        for (uint32_t row = y; row < bottom; ++row) {
            for (uint32_t column = x; column < right; ++column) {
                g_target[(uint64_t)row * g_target_width + column] = color;
            }
        }
        return;
    }

    for (uint32_t damage_index = 0U;
         damage_index < g_damage_count;
         ++damage_index) {
        const fm4_damage_rect_t *damage = &g_damage[damage_index];
        uint32_t left = x > damage->x ? x : damage->x;
        uint32_t top = y > damage->y ? y : damage->y;
        uint32_t clipped_right = right < damage->x + damage->width ?
                                 right : damage->x + damage->width;
        uint32_t clipped_bottom = bottom < damage->y + damage->height ?
                                   bottom : damage->y + damage->height;
        if (left >= clipped_right || top >= clipped_bottom) continue;
        for (uint32_t row = top; row < clipped_bottom; ++row) {
            for (uint32_t column = left; column < clipped_right; ++column) {
                g_target[(uint64_t)row * g_target_width + column] = color;
            }
        }
    }
}

static void draw_text(uint32_t x, uint32_t y, const char *text, uint32_t color) {
    liteos_text_draw(g_target, g_target_width, g_target_width, g_target_height,
                     (int32_t)x, (int32_t)y, text, color);
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


static bool load_directory(void);
static int32_t fm4_compare(const char *a, const char *b);
static void fm4_dialog_geometry(uint32_t *out_x, uint32_t *out_y,
                                uint32_t *out_width, uint32_t *out_height);

static void fm4_fit_text(char *text, uint32_t capacity,
                         uint32_t maximum_width) {
    uint32_t length;

    if (text == 0 || capacity == 0U) {
        return;
    }
    length = text_length(text);
    if (length >= capacity) length = capacity - 1U;
    if (liteos_text_measure(text) <= maximum_width) return;
    for (uint32_t visible = length; visible > 0U; --visible) {
        if (visible <= 3U) {
            for (uint32_t index = 0U; index < visible; ++index) {
                text[index] = '.';
            }
        }
        else {
            text[visible - 3U] = '.';
            text[visible - 2U] = '.';
            text[visible - 1U] = '.';
        }
        text[visible] = '\0';
        if (liteos_text_measure(text) <= maximum_width) return;
    }
    text[0] = '\0';
}

static void fm4_volume_label(char *destination, uint32_t capacity,
                             const char *name) {
    copy_text(destination, capacity, name);
    /* Keep sidebar labels inside the 200-pixel shelf instead of relying on
     * framebuffer clipping. */
    fm4_fit_text(destination, capacity, FM4_SIDEBAR_WIDTH - 66U);
}


enum {
    FM4_CONTROL_NONE = 0U,

    FM4_CONTROL_BACK,
    FM4_CONTROL_FORWARD,

    FM4_CONTROL_SEARCH,
    FM4_CONTROL_VIEW,
    FM4_CONTROL_SORT,
    FM4_CONTROL_CLOSE,

    /* Volume controls are allocated as FM4_CONTROL_VOLUME_BASE + index. */
    FM4_CONTROL_VOLUME_BASE = 0x100U,

    FM4_CONTROL_CONTEXT_OPEN,
    FM4_CONTROL_CONTEXT_DELETE,
    FM4_CONTROL_CONTEXT_PROPERTIES,

    FM4_CONTROL_DIALOG_CANCEL,
    FM4_CONTROL_DIALOG_DELETE,
};

static void fm4_volume_add(const char *path, const char *name) {
    if (path == 0 || name == 0 || path[0] == '\0' || name[0] == '\0' ||
        g_volume_count >= FM4_VOLUME_CAPACITY) return;

    for (uint32_t index = 0U; index < g_volume_count; ++index) {
        if (fm4_compare(g_volumes[index].path, path) == 0) return;
    }

    copy_text(g_volumes[g_volume_count].path,
              sizeof(g_volumes[g_volume_count].path), path);
    fm4_volume_label(g_volumes[g_volume_count].label,
                     sizeof(g_volumes[g_volume_count].label), name);
    ++g_volume_count;
}

/*
 * There is one VFS namespace today, while removable filesystems are exposed
 * below conventional mount directories.  Keep the UI honest by showing the
 * root filesystem plus only directories found below those mount containers;
 * ordinary folders in the user's home are never presented as disks.
 */
static void fm4_load_volumes(void) {
    static const char *const containers[] = {
        "/mnt", "/media", "/volumes", "/disks", "/drives",
    };

    g_volume_count = 0U;
    fm4_volume_add("/", "System Volume");

    for (uint32_t container_index = 0U;
         container_index < sizeof(containers) / sizeof(containers[0]);
         ++container_index) {
        os_file_info_t container_info = {0};
        if (stat_path(containers[container_index], &container_info) < 0 ||
            container_info.type != OS_FILE_TYPE_DIRECTORY) continue;

        for (uint32_t entry_index = 0U;
             entry_index < FILEMAN_ENTRY_CAPACITY;
             ++entry_index) {
            os_file_enumerate_t request = {0};
            char volume_path[FILEMAN_PATH_CAPACITY];
            char volume_label[OS_FILE_NAME_MAX];

            request.hdr.size = sizeof(request);
            request.hdr.version = OS_SYSCALL_ABI_VERSION;
            request.path = (uint64_t)(uintptr_t)containers[container_index];
            request.index = entry_index;

            int64_t status = fileman_syscall_one(
                OS_SYS_FILE_ENUMERATE, (uint64_t)&request);
            if (status == -2) break;
            if (status < 0 || request.info.type != OS_FILE_TYPE_DIRECTORY) {
                continue;
            }
            if (!join_path(volume_path, containers[container_index],
                           request.info.name)) continue;

            copy_text(volume_label, sizeof(volume_label), request.info.name);
            append_text(volume_label, sizeof(volume_label), " Volume");
            fm4_volume_add(volume_path, volume_label);
        }
    }
}

static bool fm4_volume_path_active(const fileman_volume_t *volume) {
    uint32_t base_length;
    if (volume == 0) return false;
    base_length = text_length(volume->path);
    if (base_length == 1U && volume->path[0] == '/') {
        /* Prefer a mounted child volume when the current path is below it. */
        for (uint32_t index = 1U; index < g_volume_count; ++index) {
            if (fm4_volume_path_active(&g_volumes[index])) return false;
        }
        return true;
    }
    if (text_length(g_path) < base_length) return false;
    for (uint32_t index = 0U; index < base_length; ++index) {
        if (g_path[index] != volume->path[index]) return false;
    }
    return g_path[base_length] == '\0' || g_path[base_length] == '/';
}


static bool fm4_inside(
    int32_t px,
    int32_t py,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height) {

    return
        px >= (int32_t)x &&
        py >= (int32_t)y &&
        px < (int32_t)(x + width) &&
        py < (int32_t)(y + height);
}


static void fm4_status(
    const char *text) {

    copy_text(
        g_status,
        sizeof(g_status),
        text != 0 ? text : "READY");
}


/*
 * Lightweight rounded rectangle.
 */
static void fm4_round_rect(
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height,
    uint32_t color) {

    static const uint8_t inset[8] = {
        5U, 3U, 2U, 1U,
        1U, 0U, 0U, 0U,
    };

    if (width == 0U ||
        height == 0U) {
        return;
    }

    if (width < 12U ||
        height < 12U) {

        fill_rect(
            x,
            y,
            width,
            height,
            color);

        return;
    }

    for (uint32_t row = 0U;
         row < height;
         ++row) {

        uint32_t cut = 0U;

        if (row < 8U) {
            cut = inset[row];

        } else if (
            height - 1U - row < 8U) {

            cut =
                inset[
                    height - 1U - row];
        }

        if (cut * 2U >= width) {
            continue;
        }

        fill_rect(
            x + cut,
            y + row,
            width - cut * 2U,
            1U,
            color);
    }
}


/* Fileman text uses the shared proportional Blend2D renderer. */
static void fm4_text(
    uint32_t x,
    uint32_t y,
    const char *text,
    uint32_t color) {

    /*
     * Keep every Ring3 application on the same Blend2D text renderer.
     */
    draw_text(
        x,
        y,
        text,
        color);
}

static uint32_t fm4_text_width(
    const char *text) {
    uint32_t measured = liteos_text_measure(text);
    return measured != 0U ? measured : text_length(text) * LITEOS_TEXT_WIDTH;
}

static uint32_t fm4_text_y_centered(uint32_t y, uint32_t height) {
    return height > LITEOS_TEXT_HEIGHT ?
        y + (height - LITEOS_TEXT_HEIGHT) / 2U : y;
}

static void fm4_text_box_centered(uint32_t x, uint32_t y,
                                  uint32_t width, uint32_t height,
                                  const char *text, uint32_t color) {
    uint32_t text_width = fm4_text_width(text);
    uint32_t text_x = x;

    if (text_width < width) text_x += (width - text_width) / 2U;
    fm4_text(text_x, fm4_text_y_centered(y, height), text, color);
}

static void fm4_center_text(
    uint32_t x,
    uint32_t width,
    uint32_t y,
    const char *text,
    uint32_t color) {

    uint32_t tw =
        fm4_text_width(text);

    uint32_t tx = x;

    if (tw < width) {
        tx +=
            (width - tw) / 2U;
    }

    fm4_text(
        tx,
        y,
        text,
        color);
}


/*
 * Folder icon.
 */
static void fm4_folder_icon(
    uint32_t x,
    uint32_t y) {

    /*
     * Back/top tab.
     */
    fm4_round_rect(
        x + 6U,
        y + 2U,
        36U,
        20U,
        0x00478FE5U);

    /*
     * Folder body.
     */
    fm4_round_rect(
        x + 2U,
        y + 14U,
        72U,
        46U,
        0x009BC4EAU);

    fill_rect(
        x + 5U,
        y + 14U,
        66U,
        5U,
        0x00488FE4U);

    fill_rect(
        x + 7U,
        y + 57U,
        62U,
        2U,
        0x0075A9D7U);
}


/*
 * Generic file icon.
 */
static void fm4_file_icon(
    uint32_t x,
    uint32_t y) {

    fm4_round_rect(
        x + 10U,
        y,
        50U,
        64U,
        0x00D8D8D8U);

    fm4_round_rect(
        x + 11U,
        y + 1U,
        48U,
        62U,
        0x00FAFAFAU);

    /*
     * Folded corner.
     */
    fill_rect(
        x + 43U,
        y + 1U,
        16U,
        16U,
        0x00E8E8E8U);

    for (uint32_t line = 0U;
         line < 5U;
         ++line) {

        fill_rect(
            x + 20U,
            y + 25U + line * 6U,
            28U,
            2U,
            0x00CECECEU);
    }
}

/* Small drive glyph used for every sidebar volume. */
static void fm4_volume_icon(uint32_t x, uint32_t y, bool active) {
    uint32_t color = active ? 0x005B6670U : 0x00666B71U;
    fm4_round_rect(x + 1U, y + 7U, 18U, 16U, color);
    fill_rect(x + 4U, y + 11U, 12U, 2U, 0x00ECECEFU);
    fill_rect(x + 5U, y + 18U, 2U, 2U, 0x00ECECEFU);
    fill_rect(x + 13U, y + 18U, 2U, 2U, 0x00ECECEFU);
}

static void fm4_composite_pixel(uint32_t x, uint32_t y,
                                 uint32_t color, uint8_t alpha) {
    uint32_t *destination;

    if (g_target == 0 || x >= g_target_width || y >= g_target_height ||
        alpha == 0U || !fm4_damage_contains(x, y)) return;
    destination = &g_target[(uint64_t)y * g_target_width + x];
    if (alpha == 255U) {
        *destination = color;
    } else {
        uint32_t inverse = 255U - alpha;
        uint32_t r = (((color >> 16U) & 0xFFU) * alpha +
                      ((*destination >> 16U) & 0xFFU) * inverse + 127U) / 255U;
        uint32_t g = (((color >> 8U) & 0xFFU) * alpha +
                      ((*destination >> 8U) & 0xFFU) * inverse + 127U) / 255U;
        uint32_t b = ((color & 0xFFU) * alpha +
                      (*destination & 0xFFU) * inverse + 127U) / 255U;
        *destination = (*destination & 0xFF000000U) |
                       (r << 16U) | (g << 8U) | b;
    }
}

static void fm4_composite_bitmap(uint32_t x, uint32_t y,
                                 const char (*bitmap)[17],
                                 uint32_t width, uint32_t height,
                                 uint32_t color) {
    for (uint32_t row = 0U; row < height; ++row) {
        for (uint32_t column = 0U; column < width; ++column) {
            char value = bitmap[row][column];
            uint8_t nibble;

            if (value >= '0' && value <= '9') {
                nibble = (uint8_t)(value - '0');
            } else if (value >= 'A' && value <= 'F') {
                nibble = (uint8_t)(value - 'A' + 10U);
            } else {
                nibble = 0U;
            }
            fm4_composite_pixel(x + column, y + row, color,
                                (uint8_t)(nibble * 17U));
        }
    }
}

/* 16x16 alpha rasters of the existing Adwaita symbolic actions.  They are
 * composited source-over into the app surface, preserving transparent edges
 * instead of being reconstructed from text glyphs or coarse rectangles. */
static void fm4_search_icon(uint32_t x, uint32_t y, uint32_t color) {
    static const char bitmap[16][17] = {
        "0004AEFEA4000000", "009FFFFFFF900000",
        "09FE82028EF90000", "4FE3000003EF4000",
        "AF800000008FA000", "EF200000002FE000",
        "FF000000000FF000", "EF200000002FE000",
        "AF800000008FA000", "4FE3000003EF4000",
        "09FE82028EFD0000", "009FFFFFFFDF9000",
        "0004AEFEA409F900", "0000000000009F90",
        "00000000000009E0", "0000000000000000",
    };
    fm4_composite_bitmap(x, y, bitmap, 16U, 16U, color);
}

static void fm4_chevron_icon(uint32_t x, uint32_t y, bool forward,
                             uint32_t color) {
    for (uint32_t row = 0U; row < 8U; ++row) {
        uint32_t offset = row < 4U ? row : 7U - row;
        uint32_t px = forward ? x + 5U + offset : x + 13U - offset;
        fill_rect(px, y + 4U + row, 2U, 2U, color);
    }
}

static void fm4_list_icon(uint32_t x, uint32_t y, uint32_t color) {
    static const char bitmap[16][17] = {
        "0000000000000000", "0FFFF00000000000",
        "0FFFF00FFFFFFFF0", "0FFFF00FFFFFFFF0",
        "0FFFF00000000000", "0000000000000000",
        "0FFFF00000000000", "0FFFF00FFFFFFFF0",
        "0FFFF00FFFFFFFF0", "0FFFF00000000000",
        "0000000000000000", "0FFFF00000000000",
        "0FFFF00FFFFFFFF0", "0FFFF00FFFFFFFF0",
        "0FFFF00000000000", "0000000000000000",
    };
    fm4_composite_bitmap(x, y, bitmap, 16U, 16U, color);
}

static void fm4_menu_icon(uint32_t x, uint32_t y, uint32_t color) {
    for (uint32_t row = 0U; row < 3U; ++row) {
        fill_rect(x, y + row * 6U, 18U, 2U, color);
    }
}

static void fm4_sort_icon(uint32_t x, uint32_t y, uint32_t color) {
    static const char bitmap[16][17] = {
        "0000000000000000", "00FF000000000000",
        "00FF000000000000", "00FF000000000000",
        "00FF000000000000", "00FF000000000000",
        "00FF00FFFFFFFFF0", "00FF00FFFFFFFFF0",
        "00FF000000000000", "00FF00FFFFFFF000",
        "00FF00FFFFFFF000", "00FF000000000000",
        "0FFFF0FFFFF00000", "FFFFFFFFFFF00000",
        "FFFFFF0000000000", "0000000000000000",
    };
    fm4_composite_bitmap(x, y, bitmap, 16U, 16U, color);
}

static void fm4_close_icon(uint32_t x, uint32_t y) {
    static const char bitmap[16][17] = {
        "FE30000000003EFE", "EFE300000003EFE3",
        "3EFE3000003EFE30", "03EFE30003EFE300",
        "003EFE303EFE3000", "0003EFE6EFE30000",
        "00003EFFFE300000", "000006FFF6000000",
        "00003EFFFE300000", "0003EFE6EFE30000",
        "003EFE303EFE3000", "03EFE30003EFE300",
        "3EFE3000003EFE30", "EFE300000003EFE3",
        "FE30000000003EFE", "E3000000000003EF",
    };
    fm4_round_rect(x, y, 24U, 24U, 0x00E2E2E4U);
    fm4_composite_bitmap(x + 4U, y + 4U, bitmap, 16U, 16U,
                         0x00484D53U);
}

/* Client decorations own the complete surface, including the outer frame.
 * Keep this as four thin damage-aware spans so a hover or selection update
 * never forces a full-frame repaint. */
static void fm4_window_frame(void) {
    if (g_target_width < 2U || g_target_height < 2U) return;

    fill_rect(0U, 0U, g_target_width, 1U,
              FM4_WINDOW_BORDER_COLOR);
    fill_rect(0U, g_target_height - 1U, g_target_width, 1U,
              FM4_WINDOW_BORDER_COLOR);
    fill_rect(0U, 0U, 1U, g_target_height,
              FM4_WINDOW_BORDER_COLOR);
    fill_rect(g_target_width - 1U, 0U, 1U, g_target_height,
              FM4_WINDOW_BORDER_COLOR);

    /* The header separator is part of the same client-owned frame. */
    if (g_target_height > FM4_HEADER_HEIGHT) {
        fill_rect(0U, FM4_HEADER_HEIGHT - 1U, g_target_width, 1U,
                  FM4_WINDOW_BORDER_COLOR);
    }
}


/*
 * ASCII case-insensitive ordering.
 */
static char fm4_fold(char c) {
    if (c >= 'A' &&
        c <= 'Z') {

        return
            (char)(c - 'A' + 'a');
    }

    return c;
}


static int32_t fm4_compare(
    const char *a,
    const char *b) {

    uint32_t index = 0U;

    for (;;) {
        char ca =
            fm4_fold(a[index]);

        char cb =
            fm4_fold(b[index]);

        if (ca < cb) return -1;
        if (ca > cb) return 1;

        if (ca == '\0') {
            return 0;
        }

        ++index;
    }
}


static bool fm4_entry_before(
    const fileman_entry_t *a,
    const fileman_entry_t *b) {

    bool ad =
        a->info.type ==
        OS_FILE_TYPE_DIRECTORY;

    bool bd =
        b->info.type ==
        OS_FILE_TYPE_DIRECTORY;

    if (ad != bd) {
        return ad;
    }

    return
        fm4_compare(
            a->info.name,
            b->info.name) < 0;
}


/*
 * At most 64 items, so insertion sort keeps code and memory tiny.
 */
static void fm4_sort(void) {
    for (uint32_t index = 1U;
         index < g_entry_count;
         ++index) {

        fileman_entry_t value =
            g_entries[index];

        uint32_t position =
            index;

        while (position != 0U &&
               fm4_entry_before(
                   &value,
                   &g_entries[
                       position - 1U])) {

            g_entries[position] =
                g_entries[
                    position - 1U];

            --position;
        }

        g_entries[position] =
            value;
    }
}


/*
 * Grid geometry.
 */
static uint32_t fm4_content_left(void) {
    return
        FM4_SIDEBAR_WIDTH +
        FM4_GRID_MARGIN_X;
}


static uint32_t fm4_grid_top(void) {
    return
        FM4_HEADER_HEIGHT +
        FM4_GRID_MARGIN_Y;
}


static uint32_t fm4_columns(void) {
    uint32_t left =
        fm4_content_left();

    uint32_t available;
    uint32_t columns;

    if (g_window.width <=
        left + 16U) {

        return 1U;
    }

    available =
        g_window.width -
        left -
        16U;

    columns =
        available /
        FM4_GRID_STEP_X;

    return columns != 0U ?
        columns :
        1U;
}


static uint32_t fm4_visible_rows(void) {
    uint32_t top =
        fm4_grid_top();

    uint32_t available;
    uint32_t rows;

    if (g_window.height <=
        top + 16U) {

        return 1U;
    }

    available =
        g_window.height -
        top -
        16U;

    rows =
        available /
        FM4_GRID_STEP_Y;

    return rows != 0U ?
        rows :
        1U;
}


static uint32_t fm4_total_rows(void) {
    uint32_t columns =
        fm4_columns();

    if (g_entry_count == 0U) {
        return 0U;
    }

    return
        (g_entry_count +
         columns - 1U) /
        columns;
}


static uint32_t fm4_max_scroll_row(void) {
    uint32_t total =
        fm4_total_rows();

    uint32_t visible =
        fm4_visible_rows();

    return total > visible ?
        total - visible :
        0U;
}


static void fm4_normalize_scroll(void) {
    uint32_t columns =
        fm4_columns();

    uint32_t row =
        g_first_entry /
        columns;

    uint32_t maximum =
        fm4_max_scroll_row();

    if (row > maximum) {
        row = maximum;
    }

    g_first_entry =
        row * columns;
}


static void fm4_selection_visible(void) {
    uint32_t columns;
    uint32_t visible;
    uint32_t first_row;
    uint32_t selected_row;

    if (!g_selection_valid ||
        g_entry_count == 0U) {

        fm4_normalize_scroll();
        return;
    }

    if (g_selected >=
        g_entry_count) {

        g_selected =
            g_entry_count - 1U;
    }

    columns =
        fm4_columns();

    visible =
        fm4_visible_rows();

    first_row =
        g_first_entry /
        columns;

    selected_row =
        g_selected /
        columns;

    if (selected_row < first_row) {
        first_row =
            selected_row;

    } else if (
        selected_row >=
        first_row + visible) {

        first_row =
            selected_row -
            visible + 1U;
    }

    if (first_row >
        fm4_max_scroll_row()) {

        first_row =
            fm4_max_scroll_row();
    }

    g_first_entry =
        first_row * columns;
}


static void fm4_scroll(
    int32_t rows) {

    uint32_t columns =
        fm4_columns();

    int64_t current =
        (int64_t)(
            g_first_entry /
            columns);

    int64_t next =
        current + rows;

    uint32_t maximum =
        fm4_max_scroll_row();

    if (next < 0) {
        next = 0;
    }

    if (next >
        (int64_t)maximum) {

        next = maximum;
    }

    g_first_entry =
        (uint32_t)next *
        columns;
}


static bool fm4_item_rect(
    uint32_t index,
    uint32_t *out_x,
    uint32_t *out_y) {

    uint32_t columns;
    uint32_t position;
    uint32_t row;
    uint32_t column;

    if (index >= g_entry_count ||
        index < g_first_entry) {

        return false;
    }

    columns =
        fm4_columns();

    position =
        index -
        g_first_entry;

    row =
        position /
        columns;

    column =
        position %
        columns;

    if (row >=
        fm4_visible_rows()) {

        return false;
    }

    if (out_x != 0) {
        *out_x =
            fm4_content_left() +
            column *
            FM4_GRID_STEP_X;
    }

    if (out_y != 0) {
        *out_y =
            fm4_grid_top() +
            row *
            FM4_GRID_STEP_Y;
    }

    return true;
}

static void fm4_damage_item(uint32_t index) {
    uint32_t x;
    uint32_t y;
    if (index != FM4_INDEX_NONE && fm4_item_rect(index, &x, &y)) {
        fm4_damage_rect(x, y, FM4_GRID_CARD_WIDTH, FM4_GRID_CARD_HEIGHT);
    }
}

static void fm4_damage_grid(void) {
    uint32_t y = FM4_HEADER_HEIGHT;
    uint32_t width = g_window.width > FM4_SIDEBAR_WIDTH ?
                     g_window.width - FM4_SIDEBAR_WIDTH : 0U;
    uint32_t height = g_window.height > y ? g_window.height - y : 0U;
    fm4_damage_rect(FM4_SIDEBAR_WIDTH, y, width, height);
}

static void fm4_damage_status(void) {
    uint32_t x = FM4_SIDEBAR_WIDTH + 16U;
    uint32_t y = g_window.height > 44U ? g_window.height - 44U : 0U;
    uint32_t width = g_window.width > x + 10U ?
                     g_window.width - x - 10U : 0U;
    fm4_damage_rect(x, y, width, 36U);
}

static void fm4_damage_control(uint32_t control) {
    uint32_t x;
    uint32_t y = FM4_HEADER_CONTROL_Y;
    uint32_t width = 34U;
    uint32_t height = 34U;

    switch (control) {
    case FM4_CONTROL_BACK:
        x = FM4_SIDEBAR_WIDTH + 14U;
        break;
    case FM4_CONTROL_FORWARD:
        x = FM4_SIDEBAR_WIDTH + 54U;
        break;
    case FM4_CONTROL_SEARCH:
        x = g_window.width > 140U ? g_window.width - 140U : 0U;
        width = 32U;
        height = 32U;
        break;
    case FM4_CONTROL_VIEW:
        x = g_window.width > 106U ? g_window.width - 106U : 0U;
        width = 32U;
        height = 32U;
        break;
    case FM4_CONTROL_SORT:
        x = g_window.width > 74U ? g_window.width - 74U : 0U;
        width = 32U;
        height = 32U;
        break;
    case FM4_CONTROL_CLOSE:
        x = g_window.width > 30U ? g_window.width - 30U : 0U;
        y = FM4_HEADER_CLOSE_Y;
        width = 24U;
        height = 24U;
        break;
    default:
        if (control >= FM4_CONTROL_VOLUME_BASE &&
            control < FM4_CONTROL_VOLUME_BASE + g_volume_count) {
            uint32_t index = control - FM4_CONTROL_VOLUME_BASE;
            fm4_damage_rect(0U,
                            FM4_VOLUME_FIRST_Y + index * FM4_VOLUME_ROW_HEIGHT,
                            FM4_SIDEBAR_WIDTH,
                            FM4_VOLUME_ROW_HEIGHT);
        }
        return;
    }
    fm4_damage_rect(x, y, width, height);
}

static void fm4_damage_context(void) {
    if (g_context_visible) {
        fm4_damage_rect(g_context_x, g_context_y, FM4_CONTEXT_WIDTH,
                        FM4_CONTEXT_ROW_HEIGHT * 3U);
    }
}

static void fm4_damage_dialog(void) {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    if (!g_delete_dialog) return;
    fm4_dialog_geometry(&x, &y, &width, &height);
    fm4_damage_rect(x, y, width, height);
}


static uint32_t fm4_item_at(
    int32_t px,
    int32_t py) {

    uint32_t left =
        fm4_content_left();

    uint32_t top =
        fm4_grid_top();

    uint32_t rx;
    uint32_t ry;

    uint32_t column;
    uint32_t row;

    uint32_t index;

    if (px < (int32_t)left ||
        py < (int32_t)top) {

        return FM4_INDEX_NONE;
    }

    rx =
        (uint32_t)px -
        left;

    ry =
        (uint32_t)py -
        top;

    column =
        rx /
        FM4_GRID_STEP_X;

    row =
        ry /
        FM4_GRID_STEP_Y;

    if (column >=
            fm4_columns() ||
        row >=
            fm4_visible_rows()) {

        return FM4_INDEX_NONE;
    }

    if ((rx %
         FM4_GRID_STEP_X) >=
            FM4_GRID_CARD_WIDTH ||
        (ry %
         FM4_GRID_STEP_Y) >=
            FM4_GRID_CARD_HEIGHT) {

        return FM4_INDEX_NONE;
    }

    index =
        g_first_entry +
        row * fm4_columns() +
        column;

    return index <
        g_entry_count ?
        index :
        FM4_INDEX_NONE;
}


/*
 * Filename: max two lines.
 */
static void fm4_item_label(
    uint32_t x,
    uint32_t y,
    const char *name,
    uint32_t color) {

    char first[11];
    char second[11];

    uint32_t length =
        text_length(name);

    uint32_t first_count =
        length > 10U ?
        10U :
        length;

    uint32_t second_count = 0U;

    for (uint32_t i = 0U;
         i < first_count;
         ++i) {

        first[i] =
            name[i];
    }

    first[first_count] =
        '\0';

    if (length > 10U) {
        second_count =
            length - 10U;

        if (second_count > 10U) {
            second_count = 10U;
        }

        for (uint32_t i = 0U;
             i < second_count;
             ++i) {

            second[i] =
                name[10U + i];
        }

        second[second_count] =
            '\0';

        if (length > 20U &&
            second_count >= 3U) {

            second[
                second_count - 3U] = '.';

            second[
                second_count - 2U] = '.';

            second[
                second_count - 1U] = '.';
        }
    }

    fm4_center_text(
        x,
        FM4_GRID_CARD_WIDTH,
        y,
        first,
        color);

    if (second_count != 0U) {
        fm4_center_text(
            x,
            FM4_GRID_CARD_WIDTH,
             y + LITEOS_TEXT_HEIGHT,
            second,
            color);
    }
}


/*
 * Navigation history.
 */
static void fm4_history_reset(void) {
    g_history_count = 1U;
    g_history_index = 0U;

    copy_text(
        g_history[0],
        FILEMAN_PATH_CAPACITY,
        g_path);
}


static void fm4_history_push(void) {
    if (g_history_index + 1U <
        g_history_count) {

        g_history_count =
            g_history_index + 1U;
    }

    if (g_history_count >=
        FM4_HISTORY_CAPACITY) {

        for (uint32_t i = 1U;
             i < g_history_count;
             ++i) {

            copy_text(
                g_history[i - 1U],
                FILEMAN_PATH_CAPACITY,
                g_history[i]);
        }

        --g_history_count;

        if (g_history_index != 0U) {
            --g_history_index;
        }
    }

    copy_text(
        g_history[g_history_count],
        FILEMAN_PATH_CAPACITY,
        g_path);

    g_history_index =
        g_history_count;

    ++g_history_count;
}


static bool fm4_navigate(
    const char *path,
    bool push_history) {

    char previous[
        FILEMAN_PATH_CAPACITY];

    if (path == 0 ||
        path[0] == '\0') {

        return false;
    }

    copy_text(
        previous,
        sizeof(previous),
        g_path);

    copy_text(
        g_path,
        sizeof(g_path),
        path);

    g_selection_valid = false;
    g_first_entry = 0U;

    g_context_visible = false;
    g_delete_dialog = false;

    if (!load_directory()) {
        copy_text(
            g_path,
            sizeof(g_path),
            previous);

        (void)load_directory();

        return false;
    }

    if (push_history) {
        fm4_history_push();
    }

    fm4_status("READY");

    return true;
}


static bool fm4_history_go(
    int32_t direction) {

    uint32_t next;

    if (direction < 0) {
        if (g_history_index == 0U) {
            return false;
        }

        next =
            g_history_index - 1U;

    } else {
        if (g_history_index + 1U >=
            g_history_count) {

            return false;
        }

        next =
            g_history_index + 1U;
    }

    if (!fm4_navigate(
            g_history[next],
            false)) {

        return false;
    }

    g_history_index =
        next;

    return true;
}


static void fm4_parent(void) {
    char previous[
        FILEMAN_PATH_CAPACITY];

    copy_text(
        previous,
        sizeof(previous),
        g_path);

    parent_directory();

    if (fm4_compare(
            previous,
            g_path) == 0) {

        return;
    }

    if (!load_directory()) {
        copy_text(
            g_path,
            sizeof(g_path),
            previous);

        (void)load_directory();

        return;
    }

    g_selection_valid = false;
    g_first_entry = 0U;

    fm4_history_push();
}


/*
 * Top/sidebar controls.
 */
static uint32_t fm4_control_at(
    int32_t px,
    int32_t py) {

    if (fm4_inside(
            px, py,
            FM4_SIDEBAR_WIDTH + 14U,
            FM4_HEADER_CONTROL_Y,
            34U,
            34U)) {

        return FM4_CONTROL_BACK;
    }

    if (fm4_inside(
            px, py,
            FM4_SIDEBAR_WIDTH + 54U,
            FM4_HEADER_CONTROL_Y,
            34U,
            34U)) {

        return FM4_CONTROL_FORWARD;
    }

    if (g_window.width > 140U &&
        fm4_inside(px, py, g_window.width - 140U,
                   FM4_HEADER_CONTROL_Y, 32U, 32U)) {

        return FM4_CONTROL_SEARCH;
    }

    if (g_window.width > 106U &&
        fm4_inside(px, py, g_window.width - 106U,
                   FM4_HEADER_CONTROL_Y, 32U, 32U)) {

        return FM4_CONTROL_VIEW;
    }

    if (g_window.width > 74U &&
        fm4_inside(px, py, g_window.width - 74U,
                   FM4_HEADER_CONTROL_Y, 32U, 32U)) {
        return FM4_CONTROL_SORT;
    }

    if (g_window.width > 40U &&
        fm4_inside(px, py, g_window.width - 40U,
                   FM4_HEADER_CONTROL_Y, 36U, 32U)) {
        return FM4_CONTROL_CLOSE;
    }

    for (uint32_t index = 0U; index < g_volume_count; ++index) {
        uint32_t y = FM4_VOLUME_FIRST_Y + index * FM4_VOLUME_ROW_HEIGHT;
        if (fm4_inside(px, py, 10U, y,
                       FM4_SIDEBAR_WIDTH - 20U, 35U)) {
            return FM4_CONTROL_VOLUME_BASE + index;
        }
    }

    return FM4_CONTROL_NONE;
}


/*
 * Context menu.
 */
static void fm4_show_context(
    int32_t px,
    int32_t py) {

    uint32_t x =
        px < 0 ?
        0U :
        (uint32_t)px;

    uint32_t y =
        py < 0 ?
        0U :
        (uint32_t)py;

    uint32_t height =
        FM4_CONTEXT_ROW_HEIGHT *
        3U;

    if (x + FM4_CONTEXT_WIDTH >
        g_window.width) {

        x =
            g_window.width >
            FM4_CONTEXT_WIDTH ?
            g_window.width -
            FM4_CONTEXT_WIDTH :
            0U;
    }

    if (y + height >
        g_window.height) {

        y =
            g_window.height >
            height ?
            g_window.height -
            height :
            0U;
    }

    g_context_x = x;
    g_context_y = y;

    g_context_visible = true;
}


static uint32_t fm4_context_at(
    int32_t px,
    int32_t py) {

    if (!g_context_visible ||
        !fm4_inside(
            px, py,
            g_context_x,
            g_context_y,
            FM4_CONTEXT_WIDTH,
            FM4_CONTEXT_ROW_HEIGHT *
                3U)) {

        return FM4_CONTROL_NONE;
    }

    uint32_t row =
        ((uint32_t)py -
         g_context_y) /
        FM4_CONTEXT_ROW_HEIGHT;

    if (row == 0U)
        return FM4_CONTROL_CONTEXT_OPEN;

    if (row == 1U)
        return FM4_CONTROL_CONTEXT_DELETE;

    return
        FM4_CONTROL_CONTEXT_PROPERTIES;
}


/*
 * Delete modal.
 */
static void fm4_dialog_geometry(
    uint32_t *out_x,
    uint32_t *out_y,
    uint32_t *out_width,
    uint32_t *out_height) {

    uint32_t width =
        g_window.width > 420U ?
        390U :
        (g_window.width > 32U ?
         g_window.width - 32U :
         g_window.width);

    uint32_t height = 146U;

    uint32_t x =
        g_window.width > width ?
        (g_window.width - width) /
        2U :
        0U;

    uint32_t y =
        g_window.height > height ?
        (g_window.height - height) /
        2U :
        0U;

    if (out_x) *out_x = x;
    if (out_y) *out_y = y;
    if (out_width) *out_width = width;
    if (out_height) *out_height = height;
}


static uint32_t fm4_dialog_at(
    int32_t px,
    int32_t py) {

    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;

    if (!g_delete_dialog) {
        return FM4_CONTROL_NONE;
    }

    fm4_dialog_geometry(
        &x,
        &y,
        &width,
        &height);

    if (fm4_inside(
            px, py,
            x + 20U,
            y + height - 44U,
            104U,
            30U)) {

        return FM4_CONTROL_DIALOG_CANCEL;
    }

    if (fm4_inside(
            px, py,
            x + width - 124U,
            y + height - 44U,
            104U,
            30U)) {

        return FM4_CONTROL_DIALOG_DELETE;
    }

    return FM4_CONTROL_NONE;
}


static void fm4_breadcrumb(
    char *buffer,
    uint32_t capacity) {

    /* Home is not a LiteOS volume/location.  At the root, use the same
     * volume name as the sidebar; below it, show the actual VFS path. */
    if (g_path[0] == '\0' ||
        (g_path[0] == '/' && g_path[1] == '\0')) {
        copy_text(buffer, capacity, "System Volume");
    } else {
        copy_text(buffer, capacity, g_path);
    }
}


static bool load_directory(void) {
    os_file_info_t directory_info = {0};
    fm4_load_volumes();
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
    fm4_sort();
    if (g_selected >= g_entry_count) g_selected = g_entry_count == 0U ? 0U : g_entry_count - 1U;
    g_first_entry = 0U;
    fm4_status("READY");
    return true;
}

static bool create_window(void) {
    os_display_info_t display = {0};
    os_window_create_t request = {0};

    uint32_t width;
    uint32_t height;

    display.hdr.size =
        sizeof(display);

    display.hdr.version =
        OS_SYSCALL_ABI_VERSION;

    if (fileman_syscall_one(
            OS_SYS_DISPLAY_GET_INFO,
            (uint64_t)&display) < 0 ||
        display.width < 320U ||
        display.height < 240U) {

        return false;
    }

    /* Start as a clearly non-maximized centered app window.  The server still
     * reserves a full display-sized resizable surface, but the visible Files
     * rectangle starts at 75% of the display in each direction, leaving a
     * usable desktop margin and all four resize edges reachable. */
    width = display.width * 3U / 4U;
    height = display.height * 3U / 4U;
    if (width < 640U && display.width > 640U) width = 640U;
    if (height < 420U && display.height > 420U) height = 420U;

    request.hdr.size =
        sizeof(request);

    request.hdr.version =
        OS_SYSCALL_ABI_VERSION;

    request.x = (int32_t)((display.width - width) / 2U);
    request.y = (int32_t)((display.height - height) / 2U);

    request.width = width;
    request.height = height;

    request.flags = OS_WINDOW_VISIBLE |
                    OS_WINDOW_RESIZABLE |
                    OS_WINDOW_CLIENT_DECORATIONS;

    request.background =
        0x00FAFAFAU;

    request.title[0] = 'F';
    request.title[1] = 'i';
    request.title[2] = 'l';
    request.title[3] = 'e';
    request.title[4] = 's';
    request.title[5] = '\0';

    request.address =
        FILEMAN_MAP_BASE;

    if (fileman_syscall_one(
            OS_SYS_WINDOW_CREATE,
            (uint64_t)&request) != 0 ||
        request.window ==
            OS_INVALID_HANDLE ||
        request.address == 0U) {

        return false;
    }

    g_window.handle =
        request.window;

    g_window.identifier =
        request.identifier;

    g_window.width =
        request.width;

    g_window.height =
        request.height;

    g_window.pixels =
        (uint32_t *)(uintptr_t)
            request.address;

    return true;
}

static void update_window(void) {
    os_window_update_t request = {0};
    uint32_t count;

    if (!g_damage_full && g_damage_count == 0U) return;
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.identifier = g_window.identifier;
    if (g_damage_full) {
        request.width = g_window.width;
        request.height = g_window.height;
        (void)fileman_syscall_one(OS_SYS_WINDOW_UPDATE, (uint64_t)&request);
    } else {
        count = g_damage_count;
        for (uint32_t index = 0U; index < count; ++index) {
            request.x = (int32_t)g_damage[index].x;
            request.y = (int32_t)g_damage[index].y;
            request.width = g_damage[index].width;
            request.height = g_damage[index].height;
            (void)fileman_syscall_one(OS_SYS_WINDOW_UPDATE,
                                      (uint64_t)&request);
        }
    }
    fm4_damage_reset();
}

static void render(void) {
    char breadcrumb[
        FILEMAN_PATH_CAPACITY + 16U];

    g_target =
        g_window.pixels;

    g_target_width =
        g_window.width;

    g_target_height =
        g_window.height;

    if (g_target == 0 ||
        g_target_width == 0U ||
        g_target_height == 0U) {

        return;
    }

    /* A caller that has no precise invalidation falls back to a safe full
     * frame.  Interactive paths below add only the regions whose state
     * changed, so ordinary selection/hover clicks stay clipped. */
    if (!g_damage_full && g_damage_count == 0U) fm4_damage_all();

    fm4_normalize_scroll();

    /*
     * White content canvas.
     */
    if (g_damage_full) {
        fill_rect(0U, 0U, g_target_width, g_target_height, 0x00FAFAFAU);
    } else {
        for (uint32_t index = 0U; index < g_damage_count; ++index) {
            fill_rect(g_damage[index].x, g_damage[index].y,
                      g_damage[index].width, g_damage[index].height,
                      0x00FAFAFAU);
        }
    }

    /*
     * Sidebar.
     */
    fill_rect(
        0U,
        0U,
        FM4_SIDEBAR_WIDTH,
        g_target_height,
        0x00ECECEFU);

    fill_rect(
        FM4_SIDEBAR_WIDTH - 1U,
        0U,
        1U,
        g_target_height,
        0x00D2D2D5U);

    /*
     * Sidebar title.
     */
    fm4_search_icon(12U, FM4_HEADER_ICON_Y, 0x00585E64U);
    draw_text(80U, FM4_HEADER_TEXT_Y,
              "Files", 0x00303438U);
    fm4_menu_icon(FM4_SIDEBAR_WIDTH - 29U, FM4_HEADER_MENU_Y,
                  0x00585E64U);

    /*
     * Volumes only.  A selected volume gets the same soft pill used by the
     * reference design; there are no Home/Recent/Network pseudo-locations.
     */
    for (uint32_t index = 0U; index < g_volume_count; ++index) {
        uint32_t y = FM4_VOLUME_FIRST_Y + index * FM4_VOLUME_ROW_HEIGHT;
        bool active = fm4_volume_path_active(&g_volumes[index]);

        if (active || g_hover_control == FM4_CONTROL_VOLUME_BASE + index) {
            fm4_round_rect(10U, y, FM4_SIDEBAR_WIDTH - 20U, 35U,
                           active ? 0x00DCDCDFAU : 0x00E3E3E6U);
        }

        fm4_volume_icon(22U, y + 2U, active);
        fm4_text(50U, fm4_text_y_centered(y, 35U),
                 g_volumes[index].label, 0x003B3F44U);
    }

    /*
     * Header.
     */
    fill_rect(
        FM4_SIDEBAR_WIDTH,
        0U,
        g_target_width -
            FM4_SIDEBAR_WIDTH,
        FM4_HEADER_HEIGHT,
        0x00FAFAFAU);

    /*
     * Back.
     */
    if (g_hover_control ==
        FM4_CONTROL_BACK) {

        fm4_round_rect(
            FM4_SIDEBAR_WIDTH + 14U,
            FM4_HEADER_CONTROL_Y,
            34U,
            34U,
            0x00ECECECU);
    }

    fm4_chevron_icon(FM4_SIDEBAR_WIDTH + 20U, FM4_HEADER_ICON_Y, false,
                     g_history_index != 0U ? 0x0031373CU : 0x00BFC1C3U);

    /*
     * Forward.
     */
    if (g_hover_control ==
        FM4_CONTROL_FORWARD) {

        fm4_round_rect(
            FM4_SIDEBAR_WIDTH + 54U,
            FM4_HEADER_CONTROL_Y,
            34U,
            34U,
            0x00ECECECU);
    }

    fm4_chevron_icon(FM4_SIDEBAR_WIDTH + 60U, FM4_HEADER_ICON_Y, true,
                     g_history_index + 1U < g_history_count ?
                     0x0031373CU : 0x00BFC1C3U);

    /*
     * Breadcrumb pill.
     */
    {
        uint32_t x =
            FM4_SIDEBAR_WIDTH + 88U;

        uint32_t reserved =
            150U;

        uint32_t width =
            g_target_width >
                x + reserved ?
            g_target_width -
                x -
                reserved :
            90U;

        fm4_round_rect(
            x,
            FM4_HEADER_CONTROL_Y,
            width,
            34U,
            0x00E9E9E9U);

        fm4_breadcrumb(
            breadcrumb,
            sizeof(breadcrumb));

        if (width > 22U) {
            fm4_fit_text(breadcrumb, sizeof(breadcrumb), width - 22U);
        }

        fm4_text(x + 14U, FM4_HEADER_TEXT_Y,
                 breadcrumb, 0x0043484DU);
    }

    /*
     * Search / grid controls.
     */
    if (g_target_width > 140U) {
        if (g_hover_control == FM4_CONTROL_SEARCH) {
            fm4_round_rect(g_target_width - 140U, FM4_HEADER_CONTROL_Y,
                           32U, 32U,
                           0x00ECECECU);
        }
        fm4_search_icon(g_target_width - 132U, FM4_HEADER_ICON_Y,
                        0x003E444AU);

        if (g_hover_control == FM4_CONTROL_VIEW) {
            fm4_round_rect(g_target_width - 106U, FM4_HEADER_CONTROL_Y,
                           32U, 32U,
                           0x00ECECECU);
        }
        fm4_list_icon(g_target_width - 98U, FM4_HEADER_ICON_Y,
                      0x003E444AU);

        if (g_hover_control == FM4_CONTROL_SORT) {
            fm4_round_rect(g_target_width - 74U, FM4_HEADER_CONTROL_Y,
                           32U, 32U,
                           0x00ECECECU);
        }
        fm4_sort_icon(g_target_width - 66U, FM4_HEADER_ICON_Y,
                      0x003E444AU);

        fm4_close_icon(g_target_width - 30U, FM4_HEADER_CLOSE_Y);
    }

    /*
     * File grid.
     */
    for (uint32_t index =
             g_first_entry;
         index < g_entry_count;
         ++index) {

        uint32_t x;
        uint32_t y;

        if (!fm4_item_rect(
                index,
                &x,
                &y)) {

            if (index >=
                g_first_entry +
                fm4_columns() *
                fm4_visible_rows()) {

                break;
            }

            continue;
        }

        if ((g_selection_valid &&
             g_selected == index) ||
            g_hover_item == index) {

            fm4_round_rect(
                x,
                y,
                FM4_GRID_CARD_WIDTH,
                FM4_GRID_CARD_HEIGHT,
                g_selection_valid &&
                g_selected == index ?
                    0x00E8E8E8U :
                    0x00F1F1F1U);
        }

        if (g_entries[index].info.type ==
            OS_FILE_TYPE_DIRECTORY) {

            fm4_folder_icon(
                x +
                (FM4_GRID_CARD_WIDTH -
                 76U) /
                2U,
                y + 7U);

        } else {
            fm4_file_icon(
                x +
                (FM4_GRID_CARD_WIDTH -
                 70U) /
                2U,
                y + 4U);
        }

        fm4_item_label(
            x,
            y + 78U,
            g_entries[index].info.name,
            0x00363A3FU);
    }

    /*
     * Empty folder.
     */
    if (g_entry_count == 0U) {
        fm4_text_box_centered(
            FM4_SIDEBAR_WIDTH,
            fm4_grid_top() + 16U,
            g_target_width > FM4_SIDEBAR_WIDTH ?
                g_target_width - FM4_SIDEBAR_WIDTH : 0U,
            34U,
            "This folder is empty",
            0x00818589U);
    }

    /*
     * Scrollbar thumb.
     */
    if (fm4_total_rows() >
        fm4_visible_rows()) {

        uint32_t track_y =
            FM4_HEADER_HEIGHT + 8U;

        uint32_t track_h =
            g_target_height >
                track_y + 16U ?
            g_target_height -
                track_y -
                16U :
            0U;

        uint32_t thumb_h =
            track_h *
            fm4_visible_rows() /
            fm4_total_rows();

        if (thumb_h < 28U) {
            thumb_h = 28U;
        }

        if (thumb_h > track_h) {
            thumb_h = track_h;
        }

        uint32_t max_scroll =
            fm4_max_scroll_row();

        uint32_t current =
            g_first_entry /
            fm4_columns();

        uint32_t thumb_y =
            track_y;

        if (max_scroll != 0U &&
            track_h > thumb_h) {

            thumb_y +=
                (track_h - thumb_h) *
                current /
                max_scroll;
        }

        fm4_round_rect(
            g_target_width - 7U,
            thumb_y,
            4U,
            thumb_h,
            0x00C4C4C6U);
    }

    /*
     * Context menu.
     */
    if (g_context_visible) {
        static const char *const labels[] = {
            "Open",
            "Delete",
            "Properties",
        };

        fm4_round_rect(
            g_context_x,
            g_context_y,
            FM4_CONTEXT_WIDTH,
            FM4_CONTEXT_ROW_HEIGHT * 3U,
            0x00C9C9C9U);

        fm4_round_rect(
            g_context_x + 1U,
            g_context_y + 1U,
            FM4_CONTEXT_WIDTH - 2U,
            FM4_CONTEXT_ROW_HEIGHT * 3U - 2U,
            0x00FFFFFFU);

        for (uint32_t i = 0U;
             i < 3U;
             ++i) {

            fm4_text_box_centered(
                g_context_x + 16U,
                g_context_y + i * FM4_CONTEXT_ROW_HEIGHT,
                FM4_CONTEXT_WIDTH - 32U,
                FM4_CONTEXT_ROW_HEIGHT,
                labels[i],
                i == 1U ? 0x00B33D3DU : 0x00373B40U);
        }
    }

    /*
     * Delete confirmation.
     */
    if (g_delete_dialog &&
        g_selection_valid &&
        g_selected < g_entry_count) {

        uint32_t x;
        uint32_t y;
        uint32_t width;
        uint32_t height;

        fm4_dialog_geometry(
            &x,
            &y,
            &width,
            &height);

        fm4_round_rect(
            x,
            y,
            width,
            height,
            0x00C9C9C9U);

        fm4_round_rect(
            x + 1U,
            y + 1U,
            width - 2U,
            height - 2U,
            0x00FFFFFFU);

        fm4_text(
            x + 20U,
            y + 20U,
            "Delete this item?",
            0x002F3337U);

        fm4_text(
            x + 20U,
            y + 47U,
            g_entries[g_selected].
                info.name,
            0x00666B70U);

        fm4_round_rect(
            x + 20U,
            y + height - 44U,
            104U,
            30U,
            0x00ECECECU);

        fm4_text_box_centered(
            x + 20U, y + height - 44U, 104U, 30U,
            "Cancel", 0x00363A3FU);

        fm4_round_rect(
            x + width - 124U,
            y + height - 44U,
            104U,
            30U,
            0x00D94A4AU);

        fm4_text_box_centered(
            x + width - 124U, y + height - 44U, 104U, 30U,
            "Delete", 0x00FFFFFFU);
    }

    /*
     * Status toast.
     */
    if (!(g_status[0] == 'R' &&
          g_status[1] == 'E' &&
          g_status[2] == 'A' &&
          g_status[3] == 'D' &&
          g_status[4] == 'Y' &&
          g_status[5] == '\0')) {

        uint32_t x =
            FM4_SIDEBAR_WIDTH + 16U;

        uint32_t y =
            g_target_height > 44U ?
            g_target_height - 40U :
            0U;

        uint32_t width =
            fm4_text_width(
                g_status) +
            22U;

        if (x + width >
            g_target_width - 10U) {

            width =
                g_target_width >
                    x + 10U ?
                g_target_width -
                    x - 10U :
                0U;
        }

        if (width != 0U) {
            fm4_round_rect(
                x,
                y,
                width,
                28U,
                0x00E5E5E5U);

            fm4_text(x + 11U, fm4_text_y_centered(y, 28U),
                     g_status, 0x0052585EU);
        }
    }

    fm4_window_frame();

    update_window();
}

static void open_selected(void) {
    if (!g_selection_valid ||
        g_entry_count == 0U ||
        g_selected >= g_entry_count) {

        return;
    }

    if (g_entries[g_selected].info.type ==
        OS_FILE_TYPE_DIRECTORY) {

        if (!fm4_navigate(
                g_entries[g_selected].path,
                true)) {

            fm4_status(
                "OPEN DIRECTORY FAILED");
        }

        return;
    }

    {
        char *arguments[3];

        arguments[0] =
            (char *)g_notepad_name;

        arguments[1] =
            g_entries[g_selected].path;

        arguments[2] = 0;

        int64_t child_pid = fileman_syscall_one(OS_SYS_PROCESS_FORK, 0U);
        if (child_pid == 0) {
            int64_t status = fileman_syscall_three(
                OS_SYS_PROCESS_EXEC,
                (uint64_t)(uintptr_t)g_notepad_path,
                (uint64_t)(uintptr_t)arguments,
                0U);
            fileman_exit(status < 0 ? 1U : 0U);
        }
        if (child_pid < 0) {
            fm4_status("NOTEPAD LAUNCH FAILED");
        } else {
            fm4_status("OPENED IN NOTEPAD");
        }
    }
}


static bool fm4_remove_now(void) {
    os_file_path_op_t request = {0};

    if (!g_selection_valid ||
        g_selected >= g_entry_count) {

        return false;
    }

    request.hdr.size =
        sizeof(request);

    request.hdr.version =
        OS_SYSCALL_ABI_VERSION;

    request.path =
        (uint64_t)(uintptr_t)
            g_entries[g_selected].path;

    if (fileman_syscall_one(
            OS_SYS_FILE_REMOVE,
            (uint64_t)&request) < 0) {

        fm4_status(
            "REMOVE FAILED");

        return false;
    }

    g_selection_valid = false;
    g_first_entry = 0U;

    if (!load_directory()) {
        fm4_status(
            "REFRESH FAILED");

        return false;
    }

    fm4_status("READY");

    return true;
}


static void remove_selected(void) {
    if (!g_selection_valid ||
        g_selected >= g_entry_count) {

        return;
    }

    g_context_visible = false;
    g_delete_dialog = true;
}

static void handle_key(
    const os_window_event_t *event) {

    const os_input_event_t *input =
        event != 0 ?
        &event->input :
        0;

    uint32_t columns =
        fm4_columns();

    uint32_t page =
        columns *
        fm4_visible_rows();
    uint32_t old_selected = g_selected;
    uint32_t old_first_entry = g_first_entry;

    if (event == 0 ||
        event->type !=
            OS_WINDOW_EVENT_INPUT ||
        input == 0 ||
        input->type !=
            OS_INPUT_EVENT_KEY) {

        return;
    }

    if (input->code == 0xE0U ||
        input->code == 0xE4U) {

        g_ctrl =
            input->value !=
            OS_INPUT_VALUE_RELEASE;

        return;
    }

    if (input->code == 0xE1U || input->code == 0xE5U) {
        g_shift = input->value != OS_INPUT_VALUE_RELEASE;
        return;
    }

    if (input->value ==
        OS_INPUT_VALUE_RELEASE) {

        return;
    }

    if (g_ctrl && input->code == 0x2EU) {
        (void)liteos_text_adjust(1);
        fm4_damage_all();
        render();
        return;
    }

    if (g_ctrl && input->code == 0x2DU) {
        (void)liteos_text_adjust(-1);
        fm4_damage_all();
        render();
        return;
    }

    if (g_ctrl &&
        (input->code == 0x14U ||
         input->code == (uint32_t)'Q')) {

        fileman_exit(0U);
    }

    if ((g_ctrl &&
         (input->code == 0x15U ||
          input->code == (uint32_t)'R')) ||
        input->code == 0x3EU) {

        g_context_visible = false;
        g_delete_dialog = false;

        (void)load_directory();

        fm4_damage_all();
        render();
        return;
    }

    /*
     * Escape:
     * dialog -> context -> selection.
     */
    if (input->code == 0x29U) {
        fm4_damage_context();
        fm4_damage_dialog();
        fm4_damage_item(g_selection_valid ? g_selected : FM4_INDEX_NONE);
        fm4_damage_status();
        if (g_delete_dialog) {
            g_delete_dialog = false;

        } else if (
            g_context_visible) {

            g_context_visible = false;

        } else {
            g_selection_valid = false;
        }

        fm4_status("READY");

        render();
        return;
    }

    if (input->code == 0x2AU) {
        fm4_parent();
        fm4_damage_all();
        render();
        return;
    }

    if (input->code == 0x28U ||
        input->code == 0x58U) {

        open_selected();
        fm4_damage_all();
        render();
        return;
    }

    if (input->code == 0x4CU) {
        remove_selected();
        fm4_damage_all();
        render();
        return;
    }

    if (g_entry_count == 0U) {
        return;
    }

    if (!g_selection_valid) {
        g_selection_valid = true;
        g_selected = 0U;

        fm4_selection_visible();

        fm4_damage_item(g_selected);
        render();
        return;
    }

    /*
     * Grid keyboard navigation.
     */
    if (input->code == 0x50U) {
        if (g_selected != 0U)
            --g_selected;

    } else if (
        input->code == 0x4FU) {

        if (g_selected + 1U <
            g_entry_count) {

            ++g_selected;
        }

    } else if (
        input->code == 0x52U) {

        if (g_selected >= columns) {
            g_selected -= columns;
        }

    } else if (
        input->code == 0x51U) {

        if (g_selected + columns <
            g_entry_count) {

            g_selected += columns;
        }

    } else if (
        input->code == 0x4AU) {

        g_selected = 0U;

    } else if (
        input->code == 0x4DU) {

        g_selected =
            g_entry_count - 1U;

    } else if (
        input->code == 0x4BU) {

        g_selected =
            g_selected > page ?
            g_selected - page :
            0U;

    } else if (
        input->code == 0x4EU) {

        uint32_t maximum =
            g_entry_count - 1U;

        if (page >
            maximum -
            (g_selected < maximum ?
             g_selected :
             maximum)) {

            g_selected =
                maximum;

        } else {
            g_selected += page;
        }

    } else {
        return;
    }

    fm4_damage_context();
    g_context_visible = false;

    fm4_selection_visible();

    if (old_first_entry != g_first_entry) {
        fm4_damage_grid();
    } else {
        fm4_damage_item(old_selected);
        fm4_damage_item(g_selected);
    }
    render();
}

static void handle_event(
    const os_window_event_t *event) {

    uint64_t pixels;

    if (event == 0) {
        return;
    }

    if (event->type ==
        OS_WINDOW_EVENT_CLOSE_REQUEST) {

        fileman_exit(0U);
    }

    if (event->type ==
        OS_WINDOW_EVENT_RESIZE) {

        if (event->resize.width == 0U ||
            event->resize.height == 0U) {

            return;
        }

        pixels =
            (uint64_t)
            event->resize.width *
            event->resize.height;

        if (pixels >
            event->resize.buffer_size /
            sizeof(uint32_t)) {

            return;
        }

        g_window.width =
            event->resize.width;

        g_window.height =
            event->resize.height;

        fm4_normalize_scroll();
        fm4_selection_visible();

        g_context_visible = false;

        fm4_damage_all();
        render();
        return;
    }

    if (event->type !=
        OS_WINDOW_EVENT_INPUT) {

        return;
    }

    /*
     * Ring0 already converted these into client-local coordinates.
     */
    g_pointer_x =
        event->pointer_x;

    g_pointer_y =
        event->pointer_y;

    /*
     * Mouse motion / wheel.
     */
    if (event->input.type ==
        OS_INPUT_EVENT_RELATIVE) {

        if (event->input.code ==
            OS_INPUT_REL_WHEEL) {

            if (g_ctrl) {
                (void)liteos_text_adjust(event->input.value > 0 ? 1 : -1);
                fm4_damage_all();
                render();
                return;
            }

            if (event->input.value > 0) {
                fm4_scroll(-1);

            } else if (
                event->input.value < 0) {

                fm4_scroll(1);
            }

            g_context_visible = false;

            fm4_damage_grid();
            render();
            return;
        }

        if (event->input.code ==
                OS_INPUT_REL_X ||
            event->input.code ==
                OS_INPUT_REL_Y) {

            uint32_t old_item =
                g_hover_item;

            uint32_t old_control =
                g_hover_control;

            g_hover_item =
                fm4_item_at(
                    g_pointer_x,
                    g_pointer_y);

            g_hover_control =
                fm4_control_at(
                    g_pointer_x,
                    g_pointer_y);

            /*
             * A double-click must stay on the same item.
             */
            if (g_last_click_item !=
                    FM4_INDEX_NONE &&
                g_hover_item !=
                    g_last_click_item) {

                g_last_click_item =
                    FM4_INDEX_NONE;

                g_last_click_time =
                    0U;
            }

            if (old_item !=
                    g_hover_item ||
                old_control !=
                    g_hover_control) {
                fm4_damage_item(old_item);
                fm4_damage_item(g_hover_item);
                fm4_damage_control(old_control);
                fm4_damage_control(g_hover_control);
                render();
            }

            return;
        }
    }

    /*
     * Keyboard.
     */
    if (event->input.type ==
        OS_INPUT_EVENT_KEY) {

        handle_key(event);
        return;
    }

    if (event->input.type !=
        OS_INPUT_EVENT_BUTTON) {

        return;
    }

    if (event->input.value !=
        OS_INPUT_VALUE_PRESS) {

        return;
    }

    /*
     * Right-click item -> context menu.
     */
    if (event->input.code ==
        OS_INPUT_BUTTON_RIGHT) {

        uint32_t item =
            fm4_item_at(
                g_pointer_x,
                g_pointer_y);

        if (item !=
            FM4_INDEX_NONE) {
            fm4_damage_context();
            fm4_damage_item(g_selection_valid ? g_selected : FM4_INDEX_NONE);

            g_selected = item;
            g_selection_valid = true;

            fm4_show_context(
                g_pointer_x,
                g_pointer_y);

            fm4_damage_item(item);
            fm4_damage_context();
            render();
        }

        return;
    }

    if (event->input.code !=
        OS_INPUT_BUTTON_LEFT) {

        return;
    }

    /*
     * Delete modal owns left-click input.
     */
    if (g_delete_dialog) {
        fm4_damage_dialog();
        uint32_t control =
            fm4_dialog_at(
                g_pointer_x,
                g_pointer_y);

        if (control ==
            FM4_CONTROL_DIALOG_CANCEL) {

            g_delete_dialog = false;

        } else if (
            control ==
            FM4_CONTROL_DIALOG_DELETE) {

            g_delete_dialog = false;

            if (fm4_remove_now()) fm4_damage_all();
            else fm4_damage_status();
        }

        render();
        return;
    }

    /*
     * Context menu.
     */
    if (g_context_visible) {
        fm4_damage_context();
        uint32_t control =
            fm4_context_at(
                g_pointer_x,
                g_pointer_y);

        g_context_visible = false;

        if (control ==
            FM4_CONTROL_CONTEXT_OPEN) {

            open_selected();

        } else if (
            control ==
            FM4_CONTROL_CONTEXT_DELETE) {

            if (g_selection_valid) {
                g_delete_dialog = true;
            }

        } else if (
            control ==
            FM4_CONTROL_CONTEXT_PROPERTIES) {

            if (g_selection_valid &&
                g_selected <
                g_entry_count) {

                if (g_entries[g_selected].
                        info.type ==
                    OS_FILE_TYPE_DIRECTORY) {

                    fm4_status(
                        "FOLDER");

                } else {
                    char message[128];

                    message[0] = '\0';

                    append_text(
                        message,
                        sizeof(message),
                        "FILE  ");

                    append_decimal(
                        message,
                        sizeof(message),
                        g_entries[g_selected].
                            info.size);

                    append_text(
                        message,
                        sizeof(message),
                        " B");

                    fm4_status(
                        message);
                }
            }
        }

        if (control == FM4_CONTROL_CONTEXT_OPEN) {
            fm4_damage_all();
        } else {
            fm4_damage_context();
            fm4_damage_dialog();
            fm4_damage_status();
        }
        render();
        return;
    }

    /*
     * Sidebar/header controls.
     */
    {
        uint32_t control =
            fm4_control_at(
                g_pointer_x,
                g_pointer_y);

        if (control !=
            FM4_CONTROL_NONE) {

            if (control ==
                FM4_CONTROL_BACK) {

                (void)fm4_history_go(-1);
                fm4_damage_all();

            } else if (
                control ==
                FM4_CONTROL_FORWARD) {

                (void)fm4_history_go(1);
                fm4_damage_all();

            } else if (
                control ==
                FM4_CONTROL_SEARCH) {

                fm4_status(
                    "SEARCH NOT IMPLEMENTED");
                fm4_damage_status();

            } else if (
                control ==
                FM4_CONTROL_VIEW) {

                fm4_status(
                    "GRID VIEW");
                fm4_damage_status();

            } else if (control == FM4_CONTROL_SORT) {
                fm4_status("SORTED BY NAME");
                fm4_damage_status();

            } else if (control == FM4_CONTROL_CLOSE) {
                fileman_exit(0U);

            } else if (control >= FM4_CONTROL_VOLUME_BASE &&
                       control < FM4_CONTROL_VOLUME_BASE + g_volume_count) {
                uint32_t volume_index = control - FM4_CONTROL_VOLUME_BASE;
                (void)fm4_navigate(g_volumes[volume_index].path, true);
                fm4_damage_all();
            }

            render();
            return;
        }
    }

    /*
     * File grid.
     */
    {
        uint32_t item =
            fm4_item_at(
                g_pointer_x,
                g_pointer_y);

        /*
         * Blank click clears selection.
         */
        if (item ==
            FM4_INDEX_NONE) {
            fm4_damage_item(g_selection_valid ? g_selected : FM4_INDEX_NONE);
            fm4_damage_status();

            g_selection_valid = false;

            g_last_click_item =
                FM4_INDEX_NONE;

            fm4_status("READY");

            render();
            return;
        }

        /*
         * Second click on same item within 450 ms = double-click.
         */
        if (g_last_click_item ==
                item &&
            event->input.timestamp >=
                g_last_click_time &&
            event->input.timestamp -
                g_last_click_time <=
                FM4_DOUBLE_CLICK_TSC_TICKS) {

            g_selected = item;
            g_selection_valid = true;

            g_last_click_item =
                FM4_INDEX_NONE;

            g_last_click_time = 0U;

            open_selected();

            fm4_damage_all();
            render();
            return;
        }

        /*
         * First click only selects.
         */
        uint32_t previous_selected =
            g_selection_valid ? g_selected : FM4_INDEX_NONE;
        fm4_damage_item(previous_selected);
        g_selected = item;
        g_selection_valid = true;

        g_last_click_item = item;

        g_last_click_time =
            event->input.timestamp;

        fm4_status("READY");

        fm4_damage_item(item);
        fm4_damage_status();
        render();
    }
}

int main(int argc, char **argv) {
    if (!liteos_text_init(LITEOS_TEXT_DEFAULT_SIZE)) {
        fileman_exit(1U);
    }
    if (argc > 1 && (argv == 0 || argv[1] == 0 || !set_path(argv[1]))) {
        fileman_exit(1U);
    }
    if (!create_window()) fileman_exit(1U);
    if (!load_directory()) fileman_exit(1U);

    fm4_history_reset();

    g_selection_valid = false;
    g_first_entry = 0U;

    g_hover_item = FM4_INDEX_NONE;
    g_hover_control = FM4_CONTROL_NONE;

    g_context_visible = false;
    g_delete_dialog = false;

    fm4_status("READY");

    render();
#if LITEOS_REALTEST
    (void)fprintf(stderr, "LITEOS_FILEMAN_READY\n");
#endif
    for (;;) {
        os_window_event_read_t request = {0};
        request.hdr.size = sizeof(request);
        request.hdr.version = OS_SYSCALL_ABI_VERSION;
        request.identifier = g_window.identifier;
        request.timeout_ns = FILEMAN_EVENT_WAIT;
        int64_t status = fileman_syscall_one(OS_SYS_WINDOW_EVENT_READ,
                                             (uint64_t)&request);
        if (status == 0) handle_event(&request.event);
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
