#pragma once
#include "base.h"
#include "object.h"
#include "list.h"
#include "spinlock.h"

struct process;
struct security_token;

#define KOBJECT_TYPE_JOB     0x0114U
#define KOBJECT_TYPE_SESSION 0x0115U

#define RESOURCE_STATE_ACTIVE  1U
#define RESOURCE_STATE_CLOSED  2U

typedef struct job_limits {
    uint64_t memory_bytes;
    uint64_t cpu_time_ns;
    uint32_t process_count;
    uint32_t handle_count;
    uint64_t io_bytes_per_sec;
} job_limits_t;

typedef struct job {
    object_header_t object;
    spinlock_t lock;
    list_head_t processes;
    struct job *parent;
    job_limits_t limits;
    atomic_uint_fast64_t committed_bytes;
    atomic_uint_fast64_t cpu_time_ns;
} job_t;

typedef struct session {
    object_header_t object;
    uint64_t id;
    struct security_token *token;
    job_t *job;
    uint32_t state;
    uint32_t flags;
} session_t;

kstatus_t job_create(job_t *parent, const job_limits_t *limits, job_t **out);
kstatus_t job_attach_process(job_t *job, struct process *process);
kstatus_t job_detach_process(struct process *process);
kstatus_t session_create(struct security_token *token, job_t *parent_job,
                         session_t **out);
bool resource_core_self_test(void);
