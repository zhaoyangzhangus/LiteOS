#pragma once

#include <kernel/base.h>
#include <kernel/object.h>
#include <kernel/spinlock.h>

#define KOBJECT_TYPE_TIMER 0x010AU

#define TIMER_RIGHT_WAIT (1U << 31)
#define TIMER_RIGHT_ALL  TIMER_RIGHT_WAIT
#define TIMER_OBJECT_LIMIT 128U

typedef struct timer_object {
    object_header_t object;
    spinlock_t lock;
    atomic_bool canceled;
    atomic_bool fired;
    uint64_t deadline_tsc;
    uint64_t period_tsc;
} timer_object_t;

kstatus_t timer_create(uint64_t delay_ns, uint64_t period_ns,
                       timer_object_t **out);
kstatus_t timer_cancel(timer_object_t *timer);
void timer_poll(uint64_t now_tsc);
bool timer_self_test(void);
