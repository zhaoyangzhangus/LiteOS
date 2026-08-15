#pragma once
#include "base.h"
#include "object.h"
#include "list.h"
#include "refcount.h"
#include "wait.h"

struct device;
struct process;
struct vm_space;

#define KOBJECT_TYPE_GPU_ALLOCATION 0x0110U
#define KOBJECT_TYPE_GPU_FENCE      0x0111U
#define KOBJECT_TYPE_GPU_CONTEXT    0x0112U

#define GPU_CONTEXT_RIGHT_SUBMIT    (1U << 0)
#define GPU_CONTEXT_RIGHT_QUERY     (1U << 1)
#define GPU_CONTEXT_RIGHT_ALL       (GPU_CONTEXT_RIGHT_SUBMIT | GPU_CONTEXT_RIGHT_QUERY)
#define GPU_ALLOCATION_RIGHT_READ   (1U << 0)
#define GPU_ALLOCATION_RIGHT_WRITE  (1U << 1)
#define GPU_ALLOCATION_RIGHT_ALL    (GPU_ALLOCATION_RIGHT_READ | GPU_ALLOCATION_RIGHT_WRITE)
#define GPU_FENCE_RIGHT_WAIT        (1U << 31)
#define GPU_FENCE_RIGHT_SIGNAL      (1U << 0)
#define GPU_SUBMIT_BATCH_MAX        32U
#define GPU_FENCE_RIGHT_ALL         (GPU_FENCE_RIGHT_WAIT | GPU_FENCE_RIGHT_SIGNAL)

enum gpu_context_state {
    GPU_CONTEXT_RUNNING = 0,
    GPU_CONTEXT_LOST = 1,
    GPU_CONTEXT_RESETTING = 2,
};

typedef struct gpu_allocation {
    object_header_t object;
    uint64_t size;
    uint32_t domain;
    uint32_t flags;
    gpu_va_t gpu_va;
    void *backend;
    void *backing;
    paddr_t backing_phys;
    uint64_t backing_size;
    void *backing_page;
    uint8_t backing_order;
    atomic_uint pin_count;
    atomic_uint residency_state;
} gpu_allocation_t;

typedef struct gpu_fence {
    object_header_t object;
    atomic_uint_fast64_t completed_value;
    atomic_int status;
    /* signal/fail 的唯一状态转换锁；等待路径只读取原子状态。 */
    spinlock_t lock;
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

/* 一批提交共享同一个上下文锁，后端可据此一次性敲响门铃。 */
typedef struct gpu_submission {
    gpu_allocation_t *allocation;
    uint64_t command_offset;
    uint64_t command_length;
    gpu_fence_t *fence;
    uint64_t signal_value;
} gpu_submission_t;

kstatus_t gpu_allocation_create(uint64_t size, gpu_allocation_t **out);
kstatus_t gpu_context_create(struct device *device, struct process *process,
                             gpu_context_t **out);
kstatus_t gpu_fence_create(gpu_fence_t **out);
kstatus_t gpu_submit(gpu_context_t *context, gpu_allocation_t *allocation,
                     uint64_t command_offset, uint64_t command_length,
                     gpu_fence_t *fence, uint64_t signal_value);
kstatus_t gpu_submit_batch(gpu_context_t *context, const gpu_submission_t *submissions,
                           uint32_t count, uint32_t *submitted);
kstatus_t gpu_fence_signal(gpu_fence_t *fence, uint64_t value);
kstatus_t gpu_fence_fail(gpu_fence_t *fence, kstatus_t status);
kstatus_t gpu_fence_wait(gpu_fence_t *fence, uint64_t value,
                         uint64_t timeout_ns);
kstatus_t gpu_context_mark_lost(gpu_context_t *context);
kstatus_t gpu_context_reset(gpu_context_t *context);
bool gpu_core_self_test(void);
