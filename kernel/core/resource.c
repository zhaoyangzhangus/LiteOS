#include <kernel/kmem.h>
#include <kernel/process.h>
#include <kernel/resource.h>
#include <kernel/security.h>

static atomic_uint_fast64_t g_next_session_id;

static void resource_lock(spinlock_t *lock) {
    while (atomic_exchange_explicit(&lock->state, 1U,
                                    memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
}

static void resource_unlock(spinlock_t *lock) {
    atomic_store_explicit(&lock->state, 0U, memory_order_release);
}

static void resource_list_insert_tail(list_head_t *head, list_head_t *node) {
    node->next = head;
    node->prev = head->prev;
    head->prev->next = node;
    head->prev = node;
}

static void resource_list_remove(list_head_t *node) {
    node->prev->next = node->next;
    node->next->prev = node->prev;
    list_init(node);
}

static void job_destroy(void *object) {
    job_t *job = (job_t *)object;
    /* 每个挂接进程都持有 Job 引用，最后一个引用释放时列表必为空。 */
    if (job->parent != 0) object_put(job->parent);
    kfree(job);
}

static void session_destroy(void *object) {
    session_t *session = (session_t *)object;
    if (session->token != 0) object_put(session->token);
    if (session->job != 0) object_put(session->job);
    kfree(session);
}

static const object_ops_t g_job_ops = {
    .destroy = job_destroy,
    .type_name = "Job",
    .is_signaled = 0,
    .wait_value = 0,
};

static const object_ops_t g_session_ops = {
    .destroy = session_destroy,
    .type_name = "Session",
    .is_signaled = 0,
    .wait_value = 0,
};

kstatus_t job_create(job_t *parent, const job_limits_t *limits, job_t **out) {
    if (out == 0) return K_EINVAL;
    job_t *job = (job_t *)kzalloc(sizeof(*job), 0);
    if (job == 0) return K_ENOMEM;
    refcount_init(&job->object.refs, 1U);
    job->object.type = KOBJECT_TYPE_JOB;
    job->object.flags = 0U;
    job->object.ops = &g_job_ops;
    job->object.security = 0;
    atomic_init(&job->lock.state, 0U);
    list_init(&job->processes);
    job->parent = parent;
    if (parent != 0) object_get(parent);
    if (limits != 0) job->limits = *limits;
    atomic_init(&job->committed_bytes, 0U);
    atomic_init(&job->cpu_time_ns, 0U);
    *out = job;
    return K_OK;
}

kstatus_t job_attach_process(job_t *job, struct process *process) {
    if (job == 0 || process == 0) return K_EINVAL;
    resource_lock(&job->lock);
    if (process->job != 0 || process->job_node.next != &process->job_node) {
        resource_unlock(&job->lock);
        return K_EBUSY;
    }
    object_get(job);
    process->job = job;
    resource_list_insert_tail(&job->processes, &process->job_node);
    resource_unlock(&job->lock);
    return K_OK;
}

kstatus_t job_detach_process(struct process *process) {
    if (process == 0) return K_EINVAL;
    job_t *job = process->job;
    if (job == 0) return K_ENOENT;
    resource_lock(&job->lock);
    if (process->job == job && process->job_node.next != &process->job_node) {
        resource_list_remove(&process->job_node);
        process->job = 0;
    }
    resource_unlock(&job->lock);
    if (process->job == 0) {
        object_put(job);
        return K_OK;
    }
    return K_EBUSY;
}

kstatus_t session_create(struct security_token *token, job_t *parent_job,
                         session_t **out) {
    if (token == 0 || out == 0) return K_EINVAL;
    job_t *job = 0;
    kstatus_t status = job_create(parent_job, 0, &job);
    if (status != K_OK) return status;
    session_t *session = (session_t *)kzalloc(sizeof(*session), 0);
    if (session == 0) {
        object_put(job);
        return K_ENOMEM;
    }
    refcount_init(&session->object.refs, 1U);
    session->object.type = KOBJECT_TYPE_SESSION;
    session->object.flags = 0U;
    session->object.ops = &g_session_ops;
    session->object.security = 0;
    session->id = atomic_fetch_add_explicit(&g_next_session_id, 1U,
                                            memory_order_relaxed) + 1U;
    session->token = token;
    object_get(token);
    session->job = job;
    session->state = RESOURCE_STATE_ACTIVE;
    session->flags = 0U;
    *out = session;
    return K_OK;
}

bool resource_core_self_test(void) {
    security_token_t *token = 0;
    job_t *root = 0;
    job_t *child = 0;
    session_t *session = 0;
    bool success = security_token_create(0U, 0U, 0U, 0U,
                                         SECURITY_CAPABILITY_SYSTEM_ADMIN,
                                         &token) == K_OK &&
                   job_create(0, 0, &root) == K_OK &&
                   job_create(root, 0, &child) == K_OK &&
                   session_create(token, root, &session) == K_OK &&
                   token != 0 && root != 0 && child != 0 && session != 0 &&
                   root->object.type == KOBJECT_TYPE_JOB &&
                   child->parent == root && child->object.type == KOBJECT_TYPE_JOB &&
                   session->job != 0 && session->token == token &&
                   session->object.type == KOBJECT_TYPE_SESSION;
    if (session != 0) object_put(session);
    if (child != 0) object_put(child);
    if (root != 0) object_put(root);
    if (token != 0) object_put(token);
    return success;
}
