#include <kernel/mm.h>
#include <kernel/pipe.h>

struct pipe_state {
    spinlock_t lock;
    wait_queue_t read_waitq;
    wait_queue_t write_waitq;
    uint8_t *buffer;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;
    uint32_t readers;
    uint32_t writers;
    uint32_t endpoint_refs;
    uint32_t active_destroyers;
    bool released;
    pipe_endpoint_t *read_endpoint;
    pipe_endpoint_t *write_endpoint;
};

typedef struct pipe_write_context {
    pipe_state_t *state;
    size_t minimum_free;
} pipe_write_context_t;

static void pipe_lock(pipe_state_t *state) {
    spinlock_lock(&state->lock);
}

static void pipe_unlock(pipe_state_t *state) {
    spinlock_unlock(&state->lock);
}

static bool pipe_read_ready(void *context) {
    pipe_state_t *state = (pipe_state_t *)context;
    bool ready;
    pipe_lock(state);
    ready = state->count != 0U || state->writers == 0U;
    pipe_unlock(state);
    return ready;
}

static bool pipe_write_ready(void *context) {
    pipe_write_context_t *write = (pipe_write_context_t *)context;
    pipe_state_t *state = write->state;
    bool ready;
    pipe_lock(state);
    ready = state->readers == 0U ||
            state->capacity - state->count >= write->minimum_free;
    pipe_unlock(state);
    return ready;
}

static void pipe_notify_endpoint(pipe_state_t *state, bool read_end) {
    pipe_endpoint_t *endpoint;
    pipe_lock(state);
    endpoint = read_end ? state->read_endpoint : state->write_endpoint;
    if (endpoint != 0 && !object_try_get(endpoint)) endpoint = 0;
    pipe_unlock(state);
    if (endpoint != 0) {
        object_notify_signaled(endpoint);
        object_put(endpoint);
    }
}

static bool pipe_endpoint_is_signaled(const void *object) {
    const pipe_endpoint_t *endpoint = (const pipe_endpoint_t *)object;
    const pipe_state_t *state;
    bool signaled;
    if (!pipe_is_endpoint(endpoint) || endpoint->state == 0) return false;
    state = endpoint->state;
    pipe_lock((pipe_state_t *)state);
    signaled = endpoint->read_end ?
        (state->count != 0U || state->writers == 0U) :
        (state->readers == 0U || state->count < state->capacity);
    pipe_unlock((pipe_state_t *)state);
    return signaled;
}

static int64_t pipe_endpoint_wait_value(const void *object) {
    const pipe_endpoint_t *endpoint = (const pipe_endpoint_t *)object;
    const pipe_state_t *state;
    int64_t value = 0;
    if (!pipe_is_endpoint(endpoint) || endpoint->state == 0) return K_EBADF;
    state = endpoint->state;
    pipe_lock((pipe_state_t *)state);
    if (endpoint->read_end) {
        if (state->count != 0U) value |= PIPE_WAIT_READABLE;
        if (state->writers == 0U) value |= PIPE_WAIT_HUP;
    } else {
        if (state->readers == 0U) value |= PIPE_WAIT_ERROR;
        else if (state->count < state->capacity) value |= PIPE_WAIT_WRITABLE;
    }
    pipe_unlock((pipe_state_t *)state);
    return value;
}

static void pipe_endpoint_destroy(void *object) {
    pipe_endpoint_t *endpoint = (pipe_endpoint_t *)object;
    pipe_state_t *state;
    pipe_endpoint_t *peer = 0;
    bool free_state = false;

    if (endpoint == 0) return;
    state = endpoint->state;
    if (state != 0) {
        pipe_lock(state);
        ++state->active_destroyers;
        if (endpoint->read_end) {
            if (state->read_endpoint == endpoint) state->read_endpoint = 0;
            if (state->readers != 0U) --state->readers;
            peer = state->write_endpoint;
        } else {
            if (state->write_endpoint == endpoint) state->write_endpoint = 0;
            if (state->writers != 0U) --state->writers;
            peer = state->read_endpoint;
        }
        if (peer != 0 && !object_try_get(peer)) peer = 0;
        if (state->endpoint_refs != 0U) --state->endpoint_refs;
        pipe_unlock(state);

        /* Wake blocking I/O after publishing the endpoint-count change. */
        (void)wake_all(&state->read_waitq);
        (void)wake_all(&state->write_waitq);
        if (peer != 0) {
            object_notify_signaled(peer);
            object_put(peer);
        }

        pipe_lock(state);
        if (state->active_destroyers != 0U) --state->active_destroyers;
        if (state->endpoint_refs == 0U && state->active_destroyers == 0U &&
            !state->released) {
            state->released = true;
            free_state = true;
        }
        pipe_unlock(state);
        if (free_state) {
            kfree(state->buffer);
            kfree(state);
        }
    }
    kfree(endpoint);
}

static const object_ops_t g_pipe_endpoint_ops = {
    .destroy = pipe_endpoint_destroy,
    .handle_close = 0,
    .type_name = "PipeEndpoint",
    .is_signaled = pipe_endpoint_is_signaled,
    .wait_value = pipe_endpoint_wait_value,
};

static void pipe_copy_in(pipe_state_t *state, const uint8_t *source,
                         size_t length) {
    size_t first = state->capacity - state->tail;
    if (first > length) first = length;
    for (size_t index = 0U; index < first; ++index) {
        state->buffer[state->tail + index] = source[index];
    }
    for (size_t index = first; index < length; ++index) {
        state->buffer[index - first] = source[index];
    }
    state->tail = (state->tail + length) % state->capacity;
    state->count += length;
}

static void pipe_copy_out(pipe_state_t *state, uint8_t *destination,
                          size_t length) {
    size_t first = state->capacity - state->head;
    if (first > length) first = length;
    for (size_t index = 0U; index < first; ++index) {
        destination[index] = state->buffer[state->head + index];
    }
    for (size_t index = first; index < length; ++index) {
        destination[index] = state->buffer[index - first];
    }
    state->head = (state->head + length) % state->capacity;
    state->count -= length;
}

kstatus_t pipe_create(uint32_t flags, pipe_endpoint_t **read_end,
                      pipe_endpoint_t **write_end) {
    pipe_state_t *state;
    pipe_endpoint_t *reader;
    pipe_endpoint_t *writer;
    if (read_end == 0 || write_end == 0 || (flags & ~OS_PIPE_FLAG_MASK) != 0U) {
        return K_EINVAL;
    }
    state = (pipe_state_t *)kzalloc(sizeof(*state), 0);
    if (state == 0) return K_ENOMEM;
    state->buffer = (uint8_t *)kmalloc(OS_PIPE_DEFAULT_SIZE, 0);
    if (state->buffer == 0) {
        kfree(state);
        return K_ENOMEM;
    }
    spinlock_init(&state->lock);
    wait_queue_init(&state->read_waitq);
    wait_queue_init(&state->write_waitq);
    state->capacity = OS_PIPE_DEFAULT_SIZE;
    state->readers = 1U;
    state->writers = 1U;
    state->endpoint_refs = 2U;

    reader = (pipe_endpoint_t *)kzalloc(sizeof(*reader), 0);
    writer = (pipe_endpoint_t *)kzalloc(sizeof(*writer), 0);
    if (reader == 0 || writer == 0) {
        kfree(writer);
        kfree(reader);
        kfree(state->buffer);
        kfree(state);
        return K_ENOMEM;
    }
    refcount_init(&reader->object.refs, 1U);
    reader->object.type = KOBJECT_TYPE_PIPE_ENDPOINT;
    reader->object.ops = &g_pipe_endpoint_ops;
    reader->state = state;
    reader->read_end = true;
    refcount_init(&writer->object.refs, 1U);
    writer->object.type = KOBJECT_TYPE_PIPE_ENDPOINT;
    writer->object.ops = &g_pipe_endpoint_ops;
    writer->state = state;
    writer->read_end = false;
    state->read_endpoint = reader;
    state->write_endpoint = writer;
    *read_end = reader;
    *write_end = writer;
    return K_OK;
}

kstatus_t pipe_read(pipe_endpoint_t *endpoint, void *buffer, size_t length,
                    uint64_t timeout_ns, uint64_t *bytes) {
    pipe_state_t *state;
    size_t amount;
    if (!pipe_is_endpoint(endpoint) || !endpoint->read_end || bytes == 0 ||
        (buffer == 0 && length != 0U)) return K_EINVAL;
    *bytes = 0U;
    if (length == 0U) return K_OK;
    state = endpoint->state;
    for (;;) {
        kstatus_t status = wait_on_queue(&state->read_waitq, pipe_read_ready,
                                         state, timeout_ns);
        if (status != K_OK) return status;
        pipe_lock(state);
        if (state->count != 0U) {
            amount = state->count < length ? state->count : length;
            pipe_copy_out(state, (uint8_t *)buffer, amount);
            pipe_unlock(state);
            *bytes = amount;
            (void)wake_all(&state->write_waitq);
            pipe_notify_endpoint(state, false);
            return K_OK;
        }
        if (state->writers == 0U) {
            pipe_unlock(state);
            return K_OK;
        }
        pipe_unlock(state);
    }
}

kstatus_t pipe_write(pipe_endpoint_t *endpoint, const void *buffer,
                     size_t length, uint64_t timeout_ns, uint64_t *bytes) {
    pipe_state_t *state;
    pipe_write_context_t write;
    size_t amount;
    if (!pipe_is_endpoint(endpoint) || endpoint->read_end || bytes == 0 ||
        (buffer == 0 && length != 0U)) return K_EINVAL;
    *bytes = 0U;
    if (length == 0U) return K_OK;
    state = endpoint->state;
    write.state = state;
    write.minimum_free = length <= OS_PIPE_BUF ? length : 1U;
    for (;;) {
        kstatus_t status = wait_on_queue(&state->write_waitq, pipe_write_ready,
                                         &write, timeout_ns);
        if (status != K_OK) return status;
        pipe_lock(state);
        if (state->readers == 0U) {
            pipe_unlock(state);
            return K_EPIPE;
        }
        if (state->capacity - state->count >= write.minimum_free) {
            amount = state->capacity - state->count;
            if (amount > length) amount = length;
            pipe_copy_in(state, (const uint8_t *)buffer, amount);
            pipe_unlock(state);
            *bytes = amount;
            (void)wake_all(&state->read_waitq);
            pipe_notify_endpoint(state, true);
            return K_OK;
        }
        pipe_unlock(state);
    }
}

kstatus_t pipe_stat(const pipe_endpoint_t *endpoint, os_file_info_t *info) {
    if (!pipe_is_endpoint(endpoint) || info == 0) return K_EBADF;
    *info = (os_file_info_t){0};
    info->type = OS_FILE_TYPE_FIFO;
    info->mode = 0666U;
    return K_OK;
}

bool pipe_is_endpoint(const void *object) {
    const object_header_t *header = (const object_header_t *)object;
    return header != 0 && header->type == KOBJECT_TYPE_PIPE_ENDPOINT &&
           header->ops == &g_pipe_endpoint_ops;
}

bool pipe_endpoint_is_read(const pipe_endpoint_t *endpoint) {
    return pipe_is_endpoint(endpoint) && endpoint->read_end;
}

int64_t pipe_wait_value(const pipe_endpoint_t *endpoint) {
    return pipe_endpoint_wait_value(endpoint);
}
