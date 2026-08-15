#pragma once
#include "base.h"
#include "list.h"
#include "spinlock.h"

struct thread;
struct wait_queue;

enum waiter_state {
    WAITER_WAITING = 0,
    WAITER_WOKEN,
    WAITER_TIMED_OUT,
    WAITER_CANCELLED,
};

typedef struct waiter {
    list_head_t node;
    list_head_t timeout_node;
    struct thread *thread;
    struct wait_queue *queue;
    atomic_uint state;
    uint32_t flags;
    uint64_t deadline_tsc;
} waiter_t;

typedef struct wait_queue {
    spinlock_t lock;
    list_head_t waiters;
    atomic_uint sequence;
} wait_queue_t;

typedef struct completion {
    atomic_uint count;
    wait_queue_t waitq;
} completion_t;

void wait_queue_init(wait_queue_t *q);
kstatus_t wait_on_queue(wait_queue_t *q, bool (*predicate)(void *), void *ctx, uint64_t timeout_ns);
uint32_t wake_one(wait_queue_t *q);
uint32_t wake_all(wait_queue_t *q);
bool wait_cancel(waiter_t *waiter);
void wait_poll_timeouts(uint64_t now_tsc);

void completion_init(completion_t *completion, uint32_t initial_count);
void complete(completion_t *completion);
kstatus_t completion_wait(completion_t *completion, uint64_t timeout_ns);
