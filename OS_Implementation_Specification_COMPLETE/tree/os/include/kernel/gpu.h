#pragma once
#include "base.h"
#include "object.h"
#include "list.h"
#include "refcount.h"
#include "wait.h"

struct device;
struct process;
struct vm_space;

typedef struct gpu_allocation {
    object_header_t object;
    uint64_t size;
    uint32_t domain;
    uint32_t flags;
    gpu_va_t gpu_va;
    void *backend;
    void *backing;
    atomic_uint pin_count;
    atomic_uint residency_state;
} gpu_allocation_t;

typedef struct gpu_fence {
    object_header_t object;
    atomic_uint_fast64_t completed_value;
    wait_queue_t waitq;
    void *backend;
} gpu_fence_t;

typedef struct gpu_context {
    object_header_t object;
    struct device *device;
    struct process *process;
    void *gpu_vm;
    void *backend;
    spinlock_t lock;
    list_head_t submissions;
    atomic_uint state;
} gpu_context_t;
