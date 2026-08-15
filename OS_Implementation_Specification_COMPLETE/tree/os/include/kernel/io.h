#pragma once
#include "base.h"
#include "object.h"
#include "list.h"
#include "wait.h"
#include "refcount.h"

struct process;
struct device;
struct file;

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

    kstatus_t status;
    uint64_t bytes_done;

    void (*cancel)(struct io_request *);
    void (*complete)(struct io_request *);

    void *completion_target;
    list_head_t queue_node;
} io_request_t;

kstatus_t io_submit(io_request_t *req);
kstatus_t io_cancel(io_request_t *req);
void io_complete(io_request_t *req, kstatus_t status, uint64_t bytes_done);
