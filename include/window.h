#ifndef LITEOS_WINDOW_H
#define LITEOS_WINDOW_H

#include "bootinfo.h"
#include "buddy.h"

#define LITEOS_WINDOW_COUNT       16U
#define LITEOS_INPUT_EVENT_COUNT  64U
#define LITEOS_WINDOW_OUTPUT_COUNT 4U
#define LITEOS_WINDOW_TITLEBAR_HEIGHT 24U

enum {
    /* Internal button state; map hardware button codes before calling the API. */
    LITEOS_WINDOW_POINTER_BUTTON_LEFT = 1U << 0,
    LITEOS_WINDOW_KEY_TAB = 0x2BU,
    LITEOS_WINDOW_KEY_LEFT_ALT = 0xE2U,
    LITEOS_WINDOW_KEY_RIGHT_ALT = 0xE6U,
};

enum {
    LITEOS_INPUT_POINTER_MOVE = 1U,
    LITEOS_INPUT_POINTER_BUTTON = 2U,
    LITEOS_INPUT_KEY = 3U,
};

typedef struct {
    UINT32 Type;
    UINT32 Code;
    INT32 X;
    INT32 Y;
    INT32 Value;
    UINT64 Timestamp;
} LITEOS_INPUT_EVENT;

typedef struct {
    UINT64 Base;
    UINT32 Width;
    UINT32 Height;
    UINT32 PixelsPerScanLine;
    UINT32 Format;
    UINT32 Mask[4];
} LITEOS_DISPLAY;

/* 一个输出对应一块独立 framebuffer；输出 0 是 UEFI 提供的主显示器。 */
typedef struct {
    BOOLEAN Active;
    BOOLEAN FullDamage;
    LITEOS_DISPLAY Display;
    UINT64 VBlankSequence;
    UINT64 PresentedVBlank;
    UINT64 MissedVBlankCount;
    UINT32 HotplugGeneration;
} LITEOS_WINDOW_OUTPUT;

typedef struct {
    BOOLEAN Used;
    BOOLEAN Visible;
    UINT32 Identifier;
    INT32 X;
    INT32 Y;
    UINT32 Width;
    UINT32 Height;
    UINT32 Background;
    LITEOS_PHYSICAL_BLOCK BufferBlock;
    UINT32 *Pixels;
} LITEOS_WINDOW;

typedef struct {
    LITEOS_DISPLAY Display;
    LITEOS_WINDOW_OUTPUT Outputs[LITEOS_WINDOW_OUTPUT_COUNT];
    UINT32 OutputCount;
    LITEOS_WINDOW Windows[LITEOS_WINDOW_COUNT];
    UINT8 ZOrder[LITEOS_WINDOW_COUNT];
    UINT32 WindowCount;
    UINT32 ZOrderCount;
    UINT32 NextIdentifier;
    BOOLEAN Dirty;
    UINT32 DirtyX;
    UINT32 DirtyY;
    UINT32 DirtyWidth;
    UINT32 DirtyHeight;
    LITEOS_INPUT_EVENT InputEvents[LITEOS_INPUT_EVENT_COUNT];
    UINT32 InputRead;
    UINT32 InputWrite;
    UINT32 InputCount;
    UINT32 FocusedIdentifier;
    UINT32 FocusCycleIdentifiers[LITEOS_WINDOW_COUNT];
    UINT32 FocusCycleCount;
    UINT32 FocusCycleIndex;
    UINT32 PointerX;
    UINT32 PointerY;
    UINT32 PointerButtons;
    UINT32 KeyboardModifiers;
    UINT32 DraggedIdentifier;
    UINT32 DragOffsetX;
    UINT32 DragOffsetY;
    UINT32 CompositorGeneration;
    UINT64 FrameSequence;
    BOOLEAN CompositorRunning;
    BOOLEAN Initialized;
} LITEOS_WINDOW_MANAGER;

BOOLEAN liteos_window_manager_init(LITEOS_WINDOW_MANAGER *manager,
                                   const LITEOS_BOOT_INFO *boot_info);
BOOLEAN liteos_window_manager_destroy(LITEOS_WINDOW_MANAGER *manager);
LITEOS_WINDOW *liteos_window_create(LITEOS_WINDOW_MANAGER *manager,
                                    INT32 x, INT32 y, UINT32 width, UINT32 height,
                                    UINT32 background, BOOLEAN visible);
BOOLEAN liteos_window_destroy(LITEOS_WINDOW_MANAGER *manager,
                              LITEOS_WINDOW *window);
BOOLEAN liteos_window_move(LITEOS_WINDOW_MANAGER *manager,
                           LITEOS_WINDOW *window, INT32 x, INT32 y);
BOOLEAN liteos_window_show(LITEOS_WINDOW_MANAGER *manager,
                           LITEOS_WINDOW *window, BOOLEAN visible);
BOOLEAN liteos_window_focus(LITEOS_WINDOW_MANAGER *manager,
                            LITEOS_WINDOW *window);
/* Selects the next visible window in a stable cycle captured after direct focus. */
BOOLEAN liteos_window_focus_next(LITEOS_WINDOW_MANAGER *manager);
UINT32 liteos_window_focused(const LITEOS_WINDOW_MANAGER *manager);
/* Returns the topmost visible window containing the screen-space point. */
LITEOS_WINDOW *liteos_window_hit_test(LITEOS_WINDOW_MANAGER *manager,
                                      INT32 x, INT32 y);
/* Relative Y is screen-space: positive values move the pointer downward. */
BOOLEAN liteos_window_pointer_move_relative(LITEOS_WINDOW_MANAGER *manager,
                                            INT32 delta_x, INT32 delta_y);
/* Button is a LITEOS_WINDOW_POINTER_BUTTON_* value, not a hardware input code. */
BOOLEAN liteos_window_pointer_button(LITEOS_WINDOW_MANAGER *manager,
                                     UINT32 button, BOOLEAN pressed);
BOOLEAN liteos_window_fill(LITEOS_WINDOW_MANAGER *manager,
                           LITEOS_WINDOW *window, UINT32 x, UINT32 y,
                           UINT32 width, UINT32 height, UINT32 color);
BOOLEAN liteos_window_present(LITEOS_WINDOW_MANAGER *manager);
BOOLEAN liteos_window_output_attach(LITEOS_WINDOW_MANAGER *manager,
                                    UINT32 output, const LITEOS_DISPLAY *display);
BOOLEAN liteos_window_output_detach(LITEOS_WINDOW_MANAGER *manager,
                                    UINT32 output);
BOOLEAN liteos_window_vblank(LITEOS_WINDOW_MANAGER *manager, UINT32 output);
UINT32 liteos_window_output_count(const LITEOS_WINDOW_MANAGER *manager);
UINT64 liteos_window_missed_vblanks(const LITEOS_WINDOW_MANAGER *manager,
                                    UINT32 output);
BOOLEAN liteos_window_compositor_restart(LITEOS_WINDOW_MANAGER *manager);
BOOLEAN liteos_window_compositor_running(const LITEOS_WINDOW_MANAGER *manager);
UINT32 liteos_window_compositor_generation(const LITEOS_WINDOW_MANAGER *manager);
UINT64 liteos_window_frame_sequence(const LITEOS_WINDOW_MANAGER *manager);
BOOLEAN liteos_input_push(LITEOS_WINDOW_MANAGER *manager,
                          const LITEOS_INPUT_EVENT *event);
BOOLEAN liteos_input_pop(LITEOS_WINDOW_MANAGER *manager,
                         LITEOS_INPUT_EVENT *event);
BOOLEAN liteos_window_pump_input(LITEOS_WINDOW_MANAGER *manager);

#endif
