#pragma once

#include "base.h"
#include "spinlock.h"
#include "wait.h"

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
bool input_core_self_test(void);
