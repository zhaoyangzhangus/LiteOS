#pragma once
#pragma once
#include "base.h"
#include "list.h"
#include "sched.h"
#include "wait.h"

typedef struct kmutex {
    wait_queue_t waitq;
    thread_t *owner;
    list_head_t owner_node;
    list_head_t pi_waiters;
    bool pi_enabled;
    bool initialized;
} kmutex_t;

void kmutex_init(kmutex_t *mutex, bool priority_inheritance);
kstatus_t kmutex_try_lock(kmutex_t *mutex);
kstatus_t kmutex_lock(kmutex_t *mutex, uint64_t timeout_ns);
kstatus_t kmutex_unlock(kmutex_t *mutex);
bool kmutex_pi_self_test(void);
