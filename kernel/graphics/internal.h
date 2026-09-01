#pragma once

#include <kernel/input.h>
#include <kernel/object.h>
#include <kernel/process.h>
#include <kernel/rect.h>
#include <kernel/shared_section.h>
#include <kernel/spinlock.h>
#include <kernel/wait.h>
#include <kernel/window_geometry.h>
#include <kernel/window_server.h>

#include "png.h"

/* REFACTOR_P7A_WINDOW_INTERNAL_OWNER: concrete Window state stays private. */

#ifndef WINDOW_DAMAGE_MAX_RECTS
#define WINDOW_DAMAGE_MAX_RECTS 16U
#endif
#ifndef WINDOW_DAMAGE_TILE_SIZE
#define WINDOW_DAMAGE_TILE_SIZE 64U
#endif
#ifndef WINDOW_DAMAGE_MAX_TILES_X
#define WINDOW_DAMAGE_MAX_TILES_X 128U
#endif
#ifndef WINDOW_DAMAGE_MAX_TILES_Y
#define WINDOW_DAMAGE_MAX_TILES_Y 128U
#endif
#ifndef WINDOW_DAMAGE_TILE_COUNT
#define WINDOW_DAMAGE_TILE_COUNT \
    (WINDOW_DAMAGE_MAX_TILES_X * WINDOW_DAMAGE_MAX_TILES_Y)
#endif
#ifndef WINDOW_DAMAGE_TILE_WORDS
#define WINDOW_DAMAGE_TILE_WORDS \
    ((WINDOW_DAMAGE_TILE_COUNT + 63U) / 64U)
#endif
#ifndef WINDOW_DAMAGE_MAX_SNAPSHOT_RECTS
#define WINDOW_DAMAGE_MAX_SNAPSHOT_RECTS 4096U
#endif
#ifndef WINDOW_RENDER_PLAN_MAX_SPANS
#define WINDOW_RENDER_PLAN_MAX_SPANS 4096U
#endif
#ifndef WINDOW_OCCLUSION_NO_FLOOR
#define WINDOW_OCCLUSION_NO_FLOOR 0xFFU
#endif

/* Keep one active drag inside one input pump.  The router owns this budget;
 * the lower-level window policy only consumes the resulting batch. */
#ifndef WINDOW_COMPOSITOR_DRAG_INPUT_EVENTS
#define WINDOW_COMPOSITOR_DRAG_INPUT_EVENTS \
    (WINDOW_EVENT_CAPACITY * 4U)
#endif

/* Private publication policy shared by the frame Owner and its scanout
 * publication Owner.  These thresholds preserve the existing transaction
 * policy while keeping the implementation out of compositor.c. */
#define WINDOW_COMPOSITOR_PUBLICATION_YIELD_PIXELS (128U * 1024U)
#define WINDOW_COMPOSITOR_ATOMIC_PIXELS (128U * 1024U)
#ifndef WINDOW_COMPOSITOR_PARALLEL_ORDINARY
#define WINDOW_COMPOSITOR_PARALLEL_ORDINARY 1U
#endif
#ifndef WINDOW_COMPOSITOR_PARALLEL_ORDINARY_PIXELS
#define WINDOW_COMPOSITOR_PARALLEL_ORDINARY_PIXELS (1024U * 1024U)
#endif
#define WINDOW_COMPOSITOR_PARALLEL_DRAG 0U

#define WINDOW_EVENT_CAPACITY     64U
#define WINDOW_SERVER_MAX_WINDOWS 64U
/* Ring0 titlebar occupies the complete window drag region. */
#define WINDOW_DRAG_REGION_HEIGHT 31U
/* 固定圆角半径，合成器使用预计算的扫描线表。 */
#define WINDOW_CORNER_RADIUS      6U

/*
 * Concrete Window state is a graphics-private object.  Public callers only
 * receive an opaque pointer and use the window-server API/accessors.
 */
struct window_server_window {
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

    void *compositor_cache;

    /* Geometry committed by the retained scene/scanout. */
    int32_t compositor_presented_x;
    int32_t compositor_presented_y;
    uint32_t compositor_presented_width;
    uint32_t compositor_presented_height;
    bool compositor_presented_valid;

    bool dirty;
    bool resize_pending;
    bool maximized;
    bool minimized;

    int32_t restore_x;
    int32_t restore_y;
    uint32_t restore_width;
    uint32_t restore_height;

    char title[32];
    os_window_event_t events[WINDOW_EVENT_CAPACITY];
    uint32_t event_read;
    uint32_t event_write;
    uint32_t event_count;
    bool event_wake_pending;
    wait_queue_t event_waitq;
};

typedef struct window_motion_batch {
    bool active;
    uint32_t device_id;
    uint16_t flags;
    uint64_t timestamp;
    int64_t delta_x;
    int64_t delta_y;
} window_motion_batch_t;

#define WINDOW_CURSOR_WIDTH 24U
#define WINDOW_CURSOR_HEIGHT 24U
#define WINDOW_CURSOR_HOTSPOT_X 3U
#define WINDOW_CURSOR_HOTSPOT_Y 1U

extern const uint32_t g_linux_cursor_argb[
    WINDOW_CURSOR_WIDTH * WINDOW_CURSOR_HEIGHT];

typedef Rect Rect;

typedef struct window_server_state {
    spinlock_t lock;
    wait_queue_t event_waitq;
    wait_queue_t worker_waitq;
    atomic_uint_fast64_t worker_generation;

    window_server_window_t *event_ready_windows[WINDOW_SERVER_MAX_WINDOWS];
    uint32_t event_ready_count;
    bool event_ready_overflow;

    window_server_window_t *windows[WINDOW_SERVER_MAX_WINDOWS];
    uint32_t count;
    uint32_t next_identifier;
    uint32_t focused_identifier;
    process_t *manager;
    uint32_t display_width;
    uint32_t display_height;
    uint32_t display_stride;
    uint32_t display_format;
    volatile uint32_t *framebuffer;
    volatile uint32_t *composite_framebuffer;
    uint32_t pointer_x;
    uint32_t pointer_y;
    uint32_t presented_pointer_x;
    uint32_t presented_pointer_y;
    bool presented_pointer_valid;
    uint32_t desktop_hovered_app;
    uint32_t desktop_pending_launch;
    uint32_t desktop_gui_mask;
    bool desktop_tab_consumed;
    bool desktop_focus_cycle_requested;
    uint32_t dragging_identifier;
    int32_t drag_offset_x;
    int32_t drag_offset_y;
    uint32_t resize_edges;
    bool drag_blit_valid;
    int32_t drag_old_x;
    int32_t drag_old_y;
    int32_t drag_new_x;
    int32_t drag_new_y;
    uint32_t drag_width;
    uint32_t drag_height;

    uint32_t title_pressed_identifier;
    uint32_t title_pressed_button;
    uint32_t title_pressed_pointer_x;
    uint32_t title_pressed_pointer_y;
    bool title_pressed_client;
    input_event_t title_pressed_event;

    thread_t worker;
    atomic_bool worker_started;
    thread_t desktop_asset_worker;
    atomic_bool desktop_asset_worker_started;
    bool kernel_ready;
    bool dirty;
    bool composing;
    bool damage_full;
    bool damage_tiles_active;
    uint32_t damage_count;
    Rect damage_rects[WINDOW_DAMAGE_MAX_RECTS];
    uint64_t damage_tiles[WINDOW_DAMAGE_TILE_WORDS];
    Rect damage_bounds;
} window_server_state_t;

typedef desktop_png_image_t desktop_asset_image_t;

extern atomic_bool g_desktop_assets_available;
extern atomic_bool g_desktop_assets_pending;
extern desktop_asset_image_t g_desktop_wallpaper_asset;
extern desktop_asset_image_t g_desktop_icons_asset;
extern desktop_asset_image_t g_desktop_file_manager_asset;
/* Immutable per-frame data owned by the compositor after capture.  Window
 * producers may continue changing the live scene once this snapshot exists. */
typedef struct compositor_window_view {
    window_server_window_t *reference;
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
    char title[32];
    bool resize_pending;
    bool maximized;
} compositor_window_view_t;

typedef struct compositor_snapshot {
    compositor_window_view_t windows[WINDOW_SERVER_MAX_WINDOWS];
    uint32_t window_count;
    uint32_t focused_identifier;
    uint32_t pointer_x;
    uint32_t pointer_y;
    uint32_t desktop_hovered_app;
    uint32_t dragging_identifier;
    bool drag_blit_valid;
    int32_t drag_old_x;
    int32_t drag_old_y;
    int32_t drag_new_x;
    int32_t drag_new_y;
    uint32_t drag_width;
    uint32_t drag_height;
    uint32_t damage_count;
    Rect damage_rects[WINDOW_DAMAGE_MAX_SNAPSHOT_RECTS];
    bool damage_full_captured;
    bool damage_tiles_captured;
    uint64_t damage_tiles[WINDOW_DAMAGE_TILE_WORDS];
    Rect damage_bounds;
} compositor_snapshot_t;

typedef struct compositor_render_span {
    Rect rect;
    uint32_t first_window;
    uint64_t window_mask;
    bool desktop_required;
} compositor_render_span_t;

typedef struct compositor_tile_meta {
    uint64_t touch_mask;
    uint64_t full_mask;
    uint64_t opaque_mask;
} compositor_tile_meta_t;

typedef struct compositor_surface_page_cache compositor_surface_page_cache_t;
typedef struct compositor_surface_source_context {
    vm_object_t *object;
    compositor_surface_page_cache_t *cache;
    uint64_t row_bytes;
    bool readable;
} compositor_surface_source_context_t;

extern compositor_snapshot_t g_compositor_snapshot;
void compositor_snapshot_test_clear_damage_tiles(void);
bool compositor_snapshot_test_set_damage_tile(uint32_t tile_index);
extern compositor_render_span_t g_compositor_render_plan[];
extern uint32_t g_compositor_render_plan_count;
extern bool g_compositor_render_plan_valid;
void compositor_render_plan_reset(void);
extern bool g_compositor_scanout_flipped;
extern compositor_tile_meta_t g_compositor_tile_meta[];
extern uint8_t g_compositor_occlusion_floor[];
extern bool g_compositor_occlusion_floor_valid;
bool compositor_tile_metadata_build(const compositor_snapshot_t *snapshot);
bool compositor_tile_metadata_valid(void);
const compositor_tile_meta_t *compositor_tile_metadata_at(uint32_t tile_x,
                                                           uint32_t tile_y);
bool compositor_tile_self_test(void);
bool compositor_snapshot_damage_tile_is_set(
    const compositor_snapshot_t *snapshot, uint32_t tile_x, uint32_t tile_y);
uint32_t compositor_snapshot_tiles_to_rects(compositor_snapshot_t *snapshot,
                                            uint32_t capacity);
bool compositor_snapshot_damage_bounds(const compositor_snapshot_t *snapshot,
                                       Rect *bounds);
void compositor_collapse_drag_damage(compositor_snapshot_t *snapshot);
bool compositor_view_intersects_snapshot_damage(
    const compositor_window_view_t *view,
    const compositor_snapshot_t *snapshot);
uint32_t compositor_snapshot_occlusion_floor(
    const compositor_snapshot_t *snapshot);
bool compositor_view_fully_covers_snapshot_bounds(
    const compositor_window_view_t *window,
    const Rect *bounds);
void compositor_build_occlusion_floor_cache(
    const compositor_snapshot_t *snapshot);
bool compositor_registry_window_intersects_damage_bounds_locked(
    const window_server_window_t *window);
bool compositor_build_render_plan(
    const compositor_snapshot_t *snapshot);
void compositor_blit_drag_overlap(void);
void desktop_draw_wallpaper_locked(void);
void desktop_shell_init(void);
bool desktop_shell_assets_available(void);
bool desktop_shell_start_asset_worker(uint32_t compositor_cpu);
bool desktop_alpha_self_test(void);
void compositor_draw_small_glyph_locked(int32_t x, int32_t y,
                                        char character, uint32_t color);
void compositor_fill_locked(int32_t x, int32_t y,
                            uint32_t width, uint32_t height,
                            uint32_t color);
void compositor_fill_rounded_locked(int32_t x, int32_t y,
                                    uint32_t width, uint32_t height,
                                    uint32_t radius, uint32_t color);
uint32_t compositor_topmost_damage_cover(void);
void compositor_fill_span_wb(volatile uint32_t *destination,
                             uint32_t pixels, uint32_t color);
void compositor_cursor_locked(void);
bool compositor_window_intersects_damage_locked(
    const compositor_window_view_t *window);
void compositor_draw_window_locked(
    const compositor_window_view_t *window);
void compositor_surface_locked(
    const compositor_window_view_t *window);
uint32_t compositor_corner_inset(uint32_t row,
                                 uint32_t width,
                                 uint32_t height);
void compositor_surface_source_prepare(
    const compositor_window_view_t *window,
    compositor_surface_source_context_t *context);
compositor_surface_page_cache_t *compositor_surface_cache_get(
    const compositor_window_view_t *view);
void compositor_copy_wb_pixels(
    uint32_t *destination,
    const uint32_t *source,
    uint32_t pixels);
bool compositor_copy_self_test(void);
uint8_t *compositor_surface_page_resolve(
    const compositor_surface_source_context_t *context,
    uint64_t page_index);
void compositor_surface_fill_destination(
    uint32_t *wb_destination,
    volatile uint32_t *device_destination,
    uint32_t pixels,
    uint32_t color);
void compositor_fill_wb_pixels(
    uint32_t *destination,
    uint32_t pixels,
    uint32_t color);
void compositor_fill_surface_pixels(
    volatile uint32_t *destination,
    uint32_t pixels,
    uint32_t color);
void compositor_surface_copy_destination(
    uint32_t *wb_destination,
    volatile uint32_t *device_destination,
    const uint32_t *source,
    uint32_t pixels);
void compositor_copy_surface_span(
    const compositor_window_view_t *window,
    const compositor_surface_source_context_t *surface,
    uint64_t source_row_offset,
    uint32_t first_column,
    uint32_t last_column,
    uint32_t *wb_destination,
    volatile uint32_t *device_destination,
    uint64_t *cached_page_index,
    uint8_t **cached_base);
bool compositor_damage_inside_surface_interior(
    const compositor_window_view_t *window);
void compositor_titlebar_locked(
    const compositor_window_view_t *window,
    uint32_t frame_color);
uint32_t compositor_occlusion_floor_for_damage(void);
void compositor_region_locked(
    const compositor_render_span_t *planned_span);
void compositor_commit_snapshot(void);
bool compositor_snapshot_overwrites_presented_cursor(void);

/* Input/resize policy shared by the event router and its hit-test unit. */
#define WINDOW_RESIZE_GRAB 3U
#define WINDOW_MIN_WIDTH 160U
#define WINDOW_MIN_HEIGHT 96U
#define WINDOW_TITLE_DRAG_DISTANCE 6U

/* Shared Ring0 decoration and drag policy constants. */
#define WINDOW_COMPOSITOR_RETAINED_DRAG 0U
#define WINDOW_COMPOSITOR_PARALLEL_WB_COPY 0U
#define DESKTOP_TOPBAR_HEIGHT 0U

enum {
    DESKTOP_APP_NONE = 0U,
    DESKTOP_APP_FILES = 1U,
    DESKTOP_APP_TERMINAL = 2U,
    DESKTOP_APP_NOTES = 3U,
    DESKTOP_APP_NETWORK = 4U,
    DESKTOP_APP_TASKMGR = 5U,
};

enum {
    WINDOW_RESIZE_LEFT = 1U << 0,
    WINDOW_RESIZE_RIGHT = 1U << 1,
    WINDOW_RESIZE_TOP = 1U << 2,
    WINDOW_RESIZE_BOTTOM = 1U << 3,
};

extern window_server_state_t g_window_server;
extern atomic_bool g_window_dirty_hint;
extern atomic_uint_fast64_t g_window_dirty_generation;
extern atomic_uint_fast64_t g_window_dirty_notified_generation;

window_server_window_t *window_scene_find_locked(uint32_t identifier);
window_server_window_t *window_scene_hit_test_locked(uint32_t x, uint32_t y);
window_server_window_t *window_scene_keyboard_locked(void);
void window_scene_set_focus_identifier_locked(uint32_t identifier);
void window_scene_focus_locked(window_server_window_t *window);
uint32_t window_input_resize_edges_locked(const window_server_window_t *window,
                                          uint32_t x, uint32_t y);
const char *window_shell_program_path(uint32_t app);
const char *window_shell_app_title(uint32_t app);
bool window_shell_title_matches(const char *title, const char *expected);
uint32_t desktop_app_at_locked(uint32_t x, uint32_t y);
void desktop_mark_app_locked(uint32_t app);
void desktop_set_hovered_app_locked(uint32_t app);

uint32_t window_damage_tile_columns_locked(void);
uint32_t window_damage_tile_rows_locked(void);
bool window_damage_tile_is_set_locked(uint32_t tile_x, uint32_t tile_y);
void window_damage_reset_locked(void);
void window_damage_clear_pending_locked(void);
void window_damage_rotate_locked(void);
void window_mark_dirty_locked(void);
void window_mark_rect_locked(int32_t x, int32_t y,
                             uint32_t width, uint32_t height);
void window_mark_window_locked(const window_server_window_t *window);
void window_mark_surface_locked(const window_server_window_t *window,
                                int32_t x, int32_t y,
                                uint32_t width, uint32_t height);
void window_mark_moved_rect_locked(int32_t old_x, int32_t old_y,
                                   int32_t new_x, int32_t new_y,
                                   uint32_t width, uint32_t height,
                                   bool render_new_position);
void window_mark_drag_corner_repair_locked(int32_t x, int32_t y,
                                           uint32_t outer_width,
                                           uint32_t outer_height,
                                           uint32_t flags);
void window_mark_moved_cursor_locked(uint32_t old_x, uint32_t old_y,
                                     uint32_t new_x, uint32_t new_y);
void window_coalesce_damage_locked(void);

/* Event-router boundary.  These calls occur once per input transaction or
 * frame boundary; none are used by the compositor's per-pixel loop. */
void window_lock(void);
void window_unlock(void);
void window_display_reset_locked(void);
void window_display_set_scanout_locked(volatile uint32_t *framebuffer);
void window_buffer_reset_locked(void);
void window_buffer_set_target_locked(volatile uint32_t *target);
void window_clear_title_capture_locked(void);
bool window_title_capture_moved_locked(void);
void window_input_reset_desktop_state_locked(void);
uint32_t window_input_take_desktop_launch_locked(void);
bool window_input_take_focus_cycle_request_locked(void);
void window_input_set_pointer_locked(uint32_t x, uint32_t y);
uint32_t window_title_button_at_locked(
    const window_server_window_t *window,
    uint32_t pointer_x,
    uint32_t pointer_y);
bool window_flush_event_wakes(void);
void window_event_reset_ready_locked(void);
void window_enqueue_event_locked(window_server_window_t *window,
                                 const input_event_t *event);
void window_enqueue_close_request_locked(window_server_window_t *window);
void window_enqueue_resize_event_locked(window_server_window_t *window);
void route_input_locked(const input_event_t *event);
void window_route_motion_axis_locked(const window_motion_batch_t *batch,
                                     uint32_t code, int64_t delta);
uint32_t window_resize_edges_locked(const window_server_window_t *window,
                                    uint32_t x, uint32_t y);
bool window_resize_locked(window_server_window_t *window,
                          int32_t delta_x, int32_t delta_y);
void window_input_reset_drag_locked(void);
void window_input_clear_drag_locked(void);
void window_input_begin_drag_locked(uint32_t identifier,
                                    int32_t offset_x,
                                    int32_t offset_y,
                                    uint32_t resize_edges);
void window_input_set_drag_blit_valid_locked(bool valid);
void window_input_record_drag_frame_locked(
    bool blit_valid,
    int32_t old_x, int32_t old_y,
    int32_t new_x, int32_t new_y,
    uint32_t width, uint32_t height);
bool window_drag_reuse_safe_locked(const window_server_window_t *window,
                                   int32_t old_x, int32_t old_y,
                                   int32_t new_x, int32_t new_y,
                                   uint32_t width, uint32_t height);
bool window_route_drag_motion_batch_locked(
    const window_motion_batch_t *batch);
void window_flush_motion_batch_locked(window_motion_batch_t *batch);
void window_route_pointer_transaction_locked(const input_event_t *event);
void desktop_cycle_window_focus(void);
bool desktop_restore_minimized_app(uint32_t app);
kstatus_t desktop_launch_program(uint32_t app);
void window_server_pump_input_mode(bool compose_now);
void compositor_frame_run(void);
uint64_t compositor_publication_damage_pixels(void);
void compositor_publication_rect(const Rect *rect, bool allow_yield);
void compositor_publication_drag_old_exposure(void);
bool compositor_snapshot_begin_locked(void);
void compositor_snapshot_plan(void);
void compositor_render_snapshot(void);
void compositor_snapshot_finish(void);
bool window_present_cursor_overlay(bool force,
                                   int64_t *out_left,
                                   int64_t *out_top,
                                   int64_t *out_right,
                                   int64_t *out_bottom);
void window_present_cursor_reset_locked(void);
void compositor_present_cursor_direct(bool force);
void compositor_qemu_repair_mark_front_cursor(
    int64_t left, int64_t top, int64_t right, int64_t bottom);
bool compositor_commit_qemu_stdvga(void);
void compositor_present_reset_scanout_state(void);
void compositor_present_mark_scanout_flipped(void);
bool compositor_present_scanout_flipped(void);
void compositor_present_init(void);
void compositor_reset_state_locked(void);
bool compositor_present_start_copy_workers(uint32_t compositor_cpu);
bool compositor_copy_rect_parallel_buffers(
    volatile uint32_t *destination,
    const uint32_t *source,
    uint32_t destination_stride,
    uint32_t source_stride,
    const Rect *rect);
void compositor_copy_wc_scanline(
    volatile uint32_t *destination,
    const uint32_t *source,
    uint32_t pixels);
bool compositor_copy_rect_parallel(const Rect *rect);

/* Window lifecycle boundary.  The registry and object lifetime are owned by
 * window.c; these cold event-queue/removal helpers remain shared until their
 * policy is moved into the corresponding graphics unit. */
extern const object_ops_t g_window_object_ops;
void window_registry_reset_locked(void);
bool window_registry_append_locked(window_server_window_t *window);
bool window_registry_remove_locked(window_server_window_t *window);
void window_registry_move_to_front_locked(window_server_window_t *window);
void remove_window_locked(window_server_window_t *window);
void window_event_schedule_wake_locked(window_server_window_t *window);
void window_server_notify_worker(void);
bool window_display_prepare(void);
void window_buffer_prepare(void);
