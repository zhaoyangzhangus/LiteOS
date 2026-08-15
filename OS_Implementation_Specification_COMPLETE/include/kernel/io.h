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

    kstatus_t status;
    uint64_t bytes_done;

    void (*cancel)(struct io_request *);
    void (*complete)(struct io_request *);

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
