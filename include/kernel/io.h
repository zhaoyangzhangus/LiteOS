#pragma once
#pragma once
#include "base.h"
#include "object.h"
#include "list.h"
#include "wait.h"
#include "refcount.h"

struct process;
struct device;
struct file;
struct completion_port;

enum io_request_state {
    IOREQ_NEW = 0,
    IOREQ_QUEUED,
    IOREQ_SUBMITTED,
    IOREQ_COMPLETING,
    IOREQ_COMPLETED,
    IOREQ_CANCELLED,
};

enum io_opcode {
    IO_READ = 1,
    IO_WRITE,
    IO_FLUSH,
    IO_IOCTL,
};

typedef struct io_vec {
    void *base;
    size_t length;
} io_vec_t;

typedef struct io_request {
    object_header_t object;
    atomic_uint state;
    uint32_t opcode;
    uint32_t flags;

    struct process *process;
    struct device *device;
    struct file *file;

    io_vec_t *vecs;
    uint32_t vec_count;
    uint32_t reserved;

    uint64_t offset;
    uint64_t deadline_ns;
    /* 内核内部提交时间戳；不属于用户态 ABI。 */
    uint64_t submit_tsc;
    /* io_submit 持有的设备对象引用和 inflight 计数标记。 */
    uint32_t device_ref_held;
    /* 设备卸载者持有的引用在终态发布前释放，保护栈请求不被复用。 */
    atomic_uint terminal_holds;

    kstatus_t status;
    uint64_t bytes_done;

    void (*cancel)(struct io_request *);
    void (*complete)(struct io_request *);
    /* Called after terminal state publication; must not access the request. */
    void (*notify)(void);

    void *completion_target;
    struct completion_port *completion_port;
    uint64_t completion_key;
    uint64_t request_id;
    /* 仅供系统调用层管理动态请求和进程生命周期。 */
    uint32_t internal_flags;
    void *private_data;
    void (*private_release)(struct io_request *);
    list_head_t queue_node;
    /* 设备核心使用的请求链表节点，用于卸载时统一取消挂起 I/O。 */
    list_head_t device_node;
} io_request_t;

static inline bool io_request_state_is_terminal(unsigned state) {
    return state == IOREQ_COMPLETED || state == IOREQ_CANCELLED;
}

static inline bool io_request_is_terminal(const io_request_t *request) {
    return request != 0 && io_request_state_is_terminal(
        atomic_load_explicit(&request->state, memory_order_acquire));
}

#define IOREQ_INTERNAL_DYNAMIC       (1U << 0)
#define IOREQ_INTERNAL_PROCESS_REF   (1U << 1)
#define IOREQ_INTERNAL_USER_REQUEST  (1U << 2)

kstatus_t io_submit(io_request_t *req);
kstatus_t io_cancel(io_request_t *req);
void io_complete(io_request_t *req, kstatus_t status, uint64_t bytes_done);
kstatus_t io_request_set_completion_port(io_request_t *req,
                                         struct completion_port *port,
                                         uint64_t user_key);

/* 将异步用户请求登记到 request id 表；返回时转移 req 的初始引用。 */
kstatus_t io_request_register_user(io_request_t *req);
/* 返回一个临时引用；完成或取消后找不到已从表中摘除的请求。 */
kstatus_t io_request_lookup_user(struct process *process, uint64_t request_id,
                                 io_request_t **out);
/* 进程退出时取消并回收仍在表中的所有用户请求。 */
void io_cancel_process(struct process *process);

/* 设备移除与用户取消共享同一 CAS 状态机，但保留各自的完成错误码。 */

void io_request_init(io_request_t *req, uint32_t opcode, struct device *device,
                     struct process *process, io_vec_t *vecs, uint32_t vec_count);

/* 设备移除与用户取消共享同一 CAS 状态机，但保留各自的完成错误码。 */
kstatus_t io_cancel_with_status(io_request_t *req, kstatus_t status);
/* 将一个观察者引用交给完成者，在终态发布前释放。 */
void io_request_hold_until_terminal(io_request_t *req);
