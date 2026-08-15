#pragma once

#include <kernel/base.h>
#include <kernel/spinlock.h>
#include <kernel/wait.h>

/* 统一输入事件：USB HID、PS/2 和未来的蓝牙输入都只向这里提交事件。 */
enum input_event_type {
    INPUT_EVENT_KEY = 1,
    INPUT_EVENT_BUTTON = 2,
    INPUT_EVENT_RELATIVE = 3,
    INPUT_EVENT_ABSOLUTE = 4,
};

enum input_event_value {
    INPUT_VALUE_RELEASE = 0,
    INPUT_VALUE_PRESS = 1,
    INPUT_VALUE_REPEAT = 2,
};

/*
 * Stable pointer codes shared by the HID backend and user-space clients.
 * Keep the values aligned with the conventional evdev relative-axis and
 * button ranges so additional input backends do not need a translation
 * layer just to report a basic mouse.
 */
enum input_relative_code {
    INPUT_REL_X = 0,
    INPUT_REL_Y = 1,
    INPUT_REL_WHEEL = 8,
};

enum input_button_code {
    INPUT_BUTTON_LEFT = 0x110,
    INPUT_BUTTON_RIGHT = 0x111,
    INPUT_BUTTON_MIDDLE = 0x112,
};

typedef struct input_event {
    uint64_t timestamp;
    uint32_t device_id;
    uint16_t type;
    uint16_t flags;
    uint32_t code;
    int32_t value;
} input_event_t;

#define INPUT_CORE_CAPACITY 256U

bool input_core_init(void);
kstatus_t input_core_push(const input_event_t *event);
kstatus_t input_core_pop(input_event_t *event);
kstatus_t input_core_read(input_event_t *event, uint64_t timeout_ns);
uint32_t input_core_pending(void);
uint64_t input_core_dropped(void);
bool input_core_self_test(void);
