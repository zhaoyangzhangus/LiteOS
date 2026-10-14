#pragma once
#pragma once

#include "abi.h"
#include "input.h"

/* 窗口由应用拥有内容缓冲，由 Ring0 Window Server 负责摆放和合成。 */
enum os_window_flags {
    OS_WINDOW_VISIBLE             = 1u << 0,
    OS_WINDOW_RESIZABLE           = 1u << 1,
    /* Ring3 owns the complete surface chrome and receives all client input. */
    OS_WINDOW_CLIENT_DECORATIONS  = 1u << 2,
    /* Surface pixels are premultiplied ARGB8888, not opaque XRGB8888. */
    OS_WINDOW_PREMULTIPLIED_ALPHA = 1u << 3,
    /* Built-in client chrome delegates its caption controls to the server. */
    OS_WINDOW_STANDARD_CAPTION_CONTROLS = 1u << 4,
    OS_WINDOW_SURFACE_EXCHANGE = 1u << 5,
    /* Client hit testing requests interactive moves explicitly; never guess
     * draggable regions from the first rows of the client surface. */
    OS_WINDOW_EXPLICIT_MOVE_RESIZE = 1u << 6,
};

/* Optional WINDOW_UPDATE metadata. Zero keeps the legacy 40px caption.
 * The surface ABI size remains unchanged. Only standard client captions
 * accept heights in [40,128]; the server uses it for paint and hit testing. */
#define OS_WINDOW_CAPTION_HEIGHT_MASK (0xFFU << 16U)
#define OS_WINDOW_CAPTION_HEIGHT(height) ((uint32_t)(height) << 16U)
static inline uint32_t os_window_caption_height(uint32_t flags) {
    uint32_t height = (flags & OS_WINDOW_CAPTION_HEIGHT_MASK) >> 16U;
    return height >= 40U && height <= 128U ? height : 40U;
}

typedef struct os_window_create {
    os_versioned_header_t hdr;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t flags;
    uint32_t background;
    char title[32];
    os_handle_t window;
    uint32_t identifier;
    uint32_t reserved;
    uint64_t address;
    uint64_t buffer_size;
} os_window_create_t;

typedef struct os_window_info {
    uint32_t identifier;
    uint32_t owner_pid;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t visible;
    uint32_t focused;
    uint32_t z_order;
    uint32_t reserved;
    uint64_t buffer_size;
    char title[32];
} os_window_info_t;

typedef struct os_window_enumerate {
    os_versioned_header_t hdr;
    uint32_t index;
    uint32_t reserved;
    os_window_info_t info;
} os_window_enumerate_t;

typedef struct os_window_map {
    os_versioned_header_t hdr;
    uint32_t identifier;
    uint32_t reserved;
    uint64_t address;
    uint64_t length;
} os_window_map_t;

/* 用户完成 surface 绘制后通知内核窗口服务器重新合成。 */
typedef struct os_window_update {
    os_versioned_header_t hdr;
    uint32_t identifier;
    uint32_t flags;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
} os_window_update_t;

/* Metadata only; the compositor owns caption rasterization and hit testing. */
typedef struct os_window_title {
    os_versioned_header_t hdr;
    uint32_t identifier;
    uint32_t reserved;
    char title[32]; /* must contain a terminating NUL */
} os_window_title_t;

typedef struct os_window_set {
    os_versioned_header_t hdr;
    uint32_t identifier;
    uint32_t visible;
    int32_t x;
    int32_t y;
    uint32_t reserved;
} os_window_set_t;

typedef struct os_window_focus {
    os_versioned_header_t hdr;
    uint32_t identifier;
    uint32_t reserved;
} os_window_focus_t;

enum os_window_event_type {
    /* INPUT remains zero so old zero-initialized event slots keep their meaning. */
    OS_WINDOW_EVENT_INPUT         = 0u,
    OS_WINDOW_EVENT_RESIZE        = 1u,

    /*
     * Ring0 decoration asks the owning application to close this window.
     *
     * This is a request, not forced destruction. The application may save
     * state, reject/delay the close, or terminate normally.
     */
    OS_WINDOW_EVENT_CLOSE_REQUEST = 2u,
    OS_WINDOW_EVENT_MINIMIZED = 3u,
    OS_WINDOW_EVENT_MOVED = 4u,
};

typedef struct os_window_resize_event {
    uint32_t width;
    uint32_t height;
    uint64_t buffer_size;
    uint64_t reserved;
} os_window_resize_event_t;

typedef struct os_window_minimized_event {
    uint32_t minimized;
    uint32_t reserved;
    uint64_t reserved2;
    uint64_t reserved3;
} os_window_minimized_event_t;

/* Absolute client origin, not the outer decoration origin. No size change. */
typedef struct os_window_moved_event {
    int32_t x;
    int32_t y;
    uint64_t reserved;
    uint64_t reserved2;
} os_window_moved_event_t;

typedef struct os_window_event {
    uint32_t identifier;
    uint32_t type;
    union {
        os_input_event_t input;
        os_window_resize_event_t resize;
        os_window_minimized_event_t minimized;
        os_window_moved_event_t moved;
    };

    /*
     * Pointer position relative to the Ring3 client surface.
     *
     * Valid for OS_WINDOW_EVENT_INPUT. Values may be negative while the
     * pointer is over Ring0-owned decorations.
     */
    int32_t pointer_x;
    int32_t pointer_y;
} os_window_event_t;

typedef struct os_window_event_read {
    os_versioned_header_t hdr;
    uint32_t identifier;
    uint32_t reserved;
    uint64_t timeout_ns;
    os_window_event_t event;
} os_window_event_read_t;

typedef struct os_window_input_dispatch {
    os_versioned_header_t hdr;
    uint32_t identifier;
    uint32_t reserved;
    os_input_event_t event;
} os_window_input_dispatch_t;
