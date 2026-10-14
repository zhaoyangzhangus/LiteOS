#pragma once
#pragma once
#include "base.h"
#include "refcount.h"
#include "wait.h"

typedef uint16_t object_type_id_t;

typedef struct object_ops {
    void (*destroy)(void *object);
    /* The owning subsystem closes logical state before a handle reference is
     * released; Object only dispatches this private lifecycle hook. */
    void (*handle_close)(void *object);
    const char *type_name;
    bool (*is_signaled)(const void *object);
    int64_t (*wait_value)(const void *object);
} object_ops_t;

typedef struct object_header {
    refcount_t refs;
    wait_queue_t signal_waiters;
    object_type_id_t type;
    uint16_t flags;
    const object_ops_t *ops;
} object_header_t;

void object_header_init(object_header_t *header, object_type_id_t type,
                        const object_ops_t *ops);
void object_get(void *object);
void object_put(void *object);
bool object_try_get(void *object);
bool object_is_signaled(const void *object);
int64_t object_wait_value(const void *object);
void object_notify_signaled(void *object);
void object_notify_changed(void *object);
kstatus_t object_wait_on_signals(void *const *objects, size_t count,
                                 bool (*predicate)(void *), void *context,
                                 uint64_t timeout_ns);
kstatus_t object_wait_on_signal(bool (*predicate)(void *), void *context,
                                uint64_t timeout_ns);
kstatus_t object_wait_many(void *const *objects, size_t count, bool wait_all,
                           uint64_t timeout_ns, uint32_t *signaled_index,
                           int64_t *wait_value);
