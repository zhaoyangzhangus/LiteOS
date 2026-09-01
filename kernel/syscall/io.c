/* REFACTOR_SYSCALL_IO_OWNER: user I/O request marshalling and lifetime. */

#include <arch/x86_64/uaccess.h>
#include <kernel/completion_port.h>
#include <kernel/device.h>
#include <kernel/io.h>
#include <kernel/kmem.h>
#include <kernel/process.h>
#include <uapi/device.h>
#include <uapi/io.h>
#include <uapi/ipc.h>

#include "internal.h"

#define USER_IO_MAX_VECTORS 64U
#define USER_IO_MAX_BYTES   (16ULL * 1024ULL * 1024ULL)

typedef struct user_io_context {
    uint32_t vector_count;
    uint32_t reserved;
    os_io_vec_t user_vectors[USER_IO_MAX_VECTORS];
    io_vec_t kernel_vectors[USER_IO_MAX_VECTORS];
    void *buffers[USER_IO_MAX_VECTORS];
} user_io_context_t;

static void user_io_context_release(io_request_t *request) {
    if (request == 0 || request->private_data == 0) return;
    user_io_context_t *context = (user_io_context_t *)request->private_data;
    for (uint32_t i = 0; i < context->vector_count; ++i) {
        if (context->buffers[i] != 0) kfree(context->buffers[i]);
    }
    kfree(context);
    request->private_data = 0;
    request->private_release = 0;
}

static void user_io_complete(io_request_t *request) {
    if (request == 0 || request->private_data == 0) return;
    user_io_context_t *context = (user_io_context_t *)request->private_data;
    if (request->opcode == IO_READ && request->status == K_OK) {
        uint64_t remaining = request->bytes_done;
        uint64_t copied = 0;
        for (uint32_t i = 0; i < context->vector_count && remaining != 0U; ++i) {
            uint64_t length = context->user_vectors[i].length;
            if (length > remaining) length = remaining;
            if (length != 0U && copy_to_user(
                    (void __user *)(uintptr_t)context->user_vectors[i].address,
                    context->buffers[i], (size_t)length) != K_OK) {
                request->status = K_EACCES;
                request->bytes_done = copied;
                break;
            }
            copied += length;
            remaining -= length;
        }
    }
    user_io_context_release(request);
}

int64_t syscall_io_submit(uint64_t arguments_pointer, uint64_t unused1,
                             uint64_t unused2, uint64_t unused3,
                             uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    os_io_submit_t arguments;
    user_io_context_t *context = 0;
    io_request_t *request = 0;
    void *device_object = 0;
    void *port_object = 0;
    kstatus_t status;
    uint64_t total = 0;
    if (process == 0) return K_EPERM;

    status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.target == OS_INVALID_HANDLE ||
        arguments.completion_port == OS_INVALID_HANDLE ||
        arguments.vector_count > USER_IO_MAX_VECTORS ||
        arguments.opcode < OS_IO_READ || arguments.opcode > OS_IO_IOCTL ||
        (arguments.vector_count != 0U && arguments.vectors == 0U)) {
        return K_EINVAL;
    }
    if ((arguments.opcode == OS_IO_READ || arguments.opcode == OS_IO_WRITE) &&
        arguments.vector_count == 0U) return K_EINVAL;
    if (arguments.vector_count != 0U &&
        !x86_user_range_valid((const void __user *)(uintptr_t)arguments.vectors,
                              (size_t)arguments.vector_count * sizeof(os_io_vec_t))) {
        return K_EINVAL;
    }

    status = handle_lookup(&process->handles, (handle_t)arguments.target,
                           OS_DEVICE_RIGHT_CONTROL, &device_object);
    if (status != K_OK) return status;
    status = handle_lookup(&process->handles, (handle_t)arguments.completion_port,
                           COMPLETION_PORT_RIGHT_WRITE, &port_object);
    if (status != K_OK) goto cleanup;
    if (((device_t *)device_object)->object.type != KOBJECT_TYPE_DEVICE ||
        ((completion_port_t *)port_object)->object.type !=
            KOBJECT_TYPE_COMPLETION_PORT) {
        status = K_EINVAL;
        goto cleanup;
    }
    if (atomic_load_explicit(&((device_t *)device_object)->state,
                             memory_order_acquire) >= DEVICE_REMOVING) {
        status = K_EDEVREMOVED;
        goto cleanup;
    }

    context = (user_io_context_t *)kzalloc(sizeof(*context), 0);
    request = (io_request_t *)kzalloc(sizeof(*request), 0);
    if (context == 0 || request == 0) {
        status = K_ENOMEM;
        goto cleanup;
    }
    context->vector_count = arguments.vector_count;
    if (arguments.vector_count != 0U && copy_from_user(
            context->user_vectors,
            (const void __user *)(uintptr_t)arguments.vectors,
            (size_t)arguments.vector_count * sizeof(os_io_vec_t)) != K_OK) {
        status = K_EACCES;
        goto cleanup;
    }
    for (uint32_t i = 0; i < context->vector_count; ++i) {
        const os_io_vec_t *user_vector = &context->user_vectors[i];
        if (user_vector->length > (uint64_t)SIZE_MAX ||
            (user_vector->length != 0U &&
             !x86_user_range_valid((const void __user *)(uintptr_t)user_vector->address,
                                   (size_t)user_vector->length)) ||
            total > USER_IO_MAX_BYTES - user_vector->length) {
            status = K_EINVAL;
            goto cleanup;
        }
        total += user_vector->length;
        context->kernel_vectors[i].length = (size_t)user_vector->length;
        if (user_vector->length == 0U) continue;
        context->buffers[i] = kzalloc((size_t)user_vector->length, 0);
        if (context->buffers[i] == 0) {
            status = K_ENOMEM;
            goto cleanup;
        }
        context->kernel_vectors[i].base = context->buffers[i];
        if (arguments.opcode == OS_IO_WRITE && copy_from_user(
                context->buffers[i],
                (const void __user *)(uintptr_t)user_vector->address,
                (size_t)user_vector->length) != K_OK) {
            status = K_EACCES;
            goto cleanup;
        }
    }
    if (total == 0U && arguments.opcode != OS_IO_FLUSH &&
        arguments.opcode != OS_IO_IOCTL) {
        status = K_EINVAL;
        goto cleanup;
    }

    io_request_init(request, arguments.opcode, (device_t *)device_object,
                    process, context->kernel_vectors, arguments.vector_count);
    request->offset = arguments.offset;
    request->internal_flags = IOREQ_INTERNAL_DYNAMIC | IOREQ_INTERNAL_PROCESS_REF;
    object_get(process);
    request->private_data = context;
    request->private_release = user_io_context_release;
    request->complete = user_io_complete;
    context = 0; /* the request owns the context after submission. */
    status = io_request_set_completion_port(
        request, (completion_port_t *)port_object, arguments.user_key);
    if (status != K_OK) goto cleanup;
    status = io_request_register_user(request);
    if (status != K_OK) goto cleanup;
    object_get(request); /* cover synchronous devices completing in io_submit. */
    uint64_t request_id = request->request_id;
    status = io_submit(request);
    object_put(request);
    request = 0;
    if (status == K_OK) {
        object_put(port_object);
        object_put(device_object);
        return (int64_t)request_id;
    }

cleanup:
    if (request != 0) {
        if ((request->internal_flags & IOREQ_INTERNAL_USER_REQUEST) != 0U &&
            !io_request_is_terminal(request)) {
            /* Remove an unsubmitted request from the user registry. */
            (void)io_cancel(request);
        }
        object_put(request);
    }
    if (context != 0) {
        for (uint32_t i = 0; i < context->vector_count; ++i) {
            if (context->buffers[i] != 0) kfree(context->buffers[i]);
        }
        kfree(context);
    }
    if (port_object != 0) object_put(port_object);
    if (device_object != 0) object_put(device_object);
    return status;
}

/* IO_CANCEL(request_id): cancel only a request owned by the current process. */
int64_t syscall_io_cancel(uint64_t request_id, uint64_t unused1,
                             uint64_t unused2, uint64_t unused3,
                             uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    io_request_t *request = 0;
    if (process == 0) return K_EPERM;
    kstatus_t status = io_request_lookup_user(process, request_id, &request);
    if (status != K_OK) return status;
    status = io_cancel(request);
    object_put(request);
    return status;
}
