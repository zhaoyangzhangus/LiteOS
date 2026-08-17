#pragma once

#include <kernel/object.h>
#include <kernel/process.h>
#include <kernel/shared_section.h>
#include <kernel/spinlock.h>
#include <kernel/wait.h>
#include <uapi/window.h>

#define KOBJECT_TYPE_WINDOW       0x0117U
#define WINDOW_RIGHT_ALL          0x00000003U
#define WINDOW_EVENT_CAPACITY     64U
#define WINDOW_SERVER_MAX_WINDOWS 64U
/* 仅作为窗口移动的默认输入区域，内核不绘制标题栏。 */
#define WINDOW_DRAG_REGION_HEIGHT 24U
/* 固定圆角半径，合成器使用预计算的扫描线表。 */
#define WINDOW_CORNER_RADIUS      6U

typedef struct window_server_window {
    object_header_t object;
    process_t *owner;
    shared_section_t *section;
    uint32_t identifier;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t flags;
    uint32_t background;
    uint64_t buffer_size;
    uint64_t owner_address;
    bool dirty;
    bool resize_pending;
    char title[32];
    os_window_event_t events[WINDOW_EVENT_CAPACITY];
    uint32_t event_read;
    uint32_t event_write;
    uint32_t event_count;
} window_server_window_t;

typedef struct window_server_snapshot {
    uint32_t identifier;
    uint32_t owner_pid;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t visible;
    uint32_t focused;
    uint32_t z_order;
    uint64_t buffer_size;
    char title[32];
} window_server_snapshot_t;

bool window_server_init(void);
/* 初始化后窗口服务器由内核持有显示输出和输入路由权。 */
bool window_server_kernel_ready(void);
kstatus_t window_server_register_manager(process_t *process);
bool window_server_is_manager(process_t *process);
kstatus_t window_server_create(process_t *owner, int32_t x, int32_t y,
                               uint32_t width, uint32_t height,
                               uint32_t flags, uint32_t background,
                               const char *title, window_server_window_t **out);
kstatus_t window_server_snapshot(uint32_t index, window_server_snapshot_t *out);
kstatus_t window_server_lookup(uint32_t identifier, window_server_window_t **out);
void window_server_put(window_server_window_t *window);
void window_server_handle_closed(window_server_window_t *window);
/* 进程退出时立即从合成列表移除该进程的全部窗口。 */
void window_server_close_process(process_t *owner);
kstatus_t window_server_set(window_server_window_t *window, int32_t x, int32_t y,
                            uint32_t visible);
kstatus_t window_server_focus(uint32_t identifier);
kstatus_t window_server_map(window_server_window_t *window, process_t *process,
                            uint64_t requested_address, uint64_t *mapped_address);
kstatus_t window_server_update(process_t *process, uint32_t identifier,
                               int32_t x, int32_t y, uint32_t width,
                               uint32_t height, uint32_t flags);
kstatus_t window_server_set_owner_address(window_server_window_t *window,
                                          uint64_t address);
kstatus_t window_server_dispatch(uint32_t identifier, const os_input_event_t *event);
kstatus_t window_server_event_read(process_t *process, uint32_t identifier,
                                   os_window_event_t *event, uint64_t timeout_ns);
/* Ring0 主循环调用：消费统一输入队列并在需要时刷新合成结果。 */
void window_server_pump_input(void);
