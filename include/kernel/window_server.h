#pragma once

#include <kernel/base.h>
#include <uapi/window.h>

struct process;

#define KOBJECT_TYPE_WINDOW       0x0117U
#define WINDOW_RIGHT_ALL          0x00000003U

/* The concrete Window object is private to kernel/graphics. */
typedef struct window_server_window window_server_window_t;

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
bool window_server_start_worker(void);
bool window_server_start_asset_worker(void);
/* Wake the compositor for a client damage submission or an input report. */
void window_server_notify_worker(void);
/* 初始化后窗口服务器由内核持有显示输出和输入路由权。 */
bool window_server_kernel_ready(void);
kstatus_t window_server_register_manager(struct process *process);
bool window_server_is_manager(struct process *process);
kstatus_t window_server_create(struct process *owner, int32_t x, int32_t y,
                               uint32_t width, uint32_t height,
                               uint32_t flags, uint32_t background,
                               const char *title, window_server_window_t **out);
kstatus_t window_server_snapshot(uint32_t index, window_server_snapshot_t *out);
kstatus_t window_server_lookup(uint32_t identifier, window_server_window_t **out);
uint32_t window_server_window_identifier(
    const window_server_window_t *window);
uint64_t window_server_window_buffer_size(
    const window_server_window_t *window);
void window_server_put(window_server_window_t *window);
void window_server_handle_closed(window_server_window_t *window);
/* 进程退出时立即从合成列表移除该进程的全部窗口。 */
void window_server_close_process(struct process *owner);
kstatus_t window_server_set(window_server_window_t *window, int32_t x, int32_t y,
                            uint32_t visible);
kstatus_t window_server_focus(uint32_t identifier);
kstatus_t window_server_map(window_server_window_t *window,
                            struct process *process,
                            uint64_t requested_address, uint64_t *mapped_address);
kstatus_t window_server_update(struct process *process, uint32_t identifier,
                               int32_t x, int32_t y, uint32_t width,
                               uint32_t height, uint32_t flags);
kstatus_t window_server_set_owner_address(window_server_window_t *window,
                                          uint64_t address);
kstatus_t window_server_dispatch(uint32_t identifier, const os_input_event_t *event);
kstatus_t window_server_event_read(struct process *process, uint32_t identifier,
                                   os_window_event_t *event, uint64_t timeout_ns);
/* Ring0 主循环调用：消费统一输入队列并在需要时刷新合成结果。 */
void window_server_pump_input(void);
bool window_input_router_self_test(void);
bool window_present_self_test(void);
bool compositor_tile_self_test(void);
bool window_lifecycle_self_test(void);
bool desktop_alpha_self_test(void);
