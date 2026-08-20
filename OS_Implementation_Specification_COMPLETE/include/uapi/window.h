#pragma once

#include "abi.h"
#include "input.h"

/* 窗口由应用拥有内容缓冲，由 Ring0 Window Server 负责摆放和合成。 */
enum os_window_flags {
    OS_WINDOW_VISIBLE             = 1u << 0,
    OS_WINDOW_RESIZABLE           = 1u << 1,
    /* Ring3 owns the complete surface chrome and receives all client input. */
    OS_WINDOW_CLIENT_DECORATIONS  = 1u << 2,
};

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
};

typedef struct os_window_resize_event {
    uint32_t width;
    uint32_t height;
    uint64_t buffer_size;
    uint64_t reserved;
} os_window_resize_event_t;

typedef struct os_window_event {
    uint32_t identifier;
    uint32_t type;
    union {
        os_input_event_t input;
        os_window_resize_event_t resize;
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
