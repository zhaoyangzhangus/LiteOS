/* REFACTOR_SYSCALL_SYNC_OWNER: IPC, timer, wait and futex handlers. */

#include <arch/x86_64/cpu.h>
#include <arch/x86_64/uaccess.h>
#include <kernel/console.h>
#include <kernel/completion_port.h>
#include <kernel/debug_stage.h>
#include <kernel/futex.h>
#include <kernel/kmem.h>
#include <kernel/message_port.h>
#include <kernel/process.h>
#include <kernel/timer.h>
#include <kernel/wait.h>
#include <uapi/ipc.h>
#include <uapi/time.h>
#include <uapi/wait.h>

#include "internal.h"

int64_t syscall_port_create(uint64_t kind, uint64_t capacity,
                               uint64_t output_pointer, uint64_t unused3,
                               uint64_t unused4, uint64_t unused5) {
    (void)unused3;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0 || kind > UINT32_MAX || capacity > UINT32_MAX) return K_EINVAL;
    void *port = 0;
    kstatus_t status;
    uint32_t rights;
    if (kind == OS_PORT_MESSAGE) {
        message_port_t *message_port = 0;
        status = message_port_create((uint32_t)capacity, &message_port);
        port = message_port;
        rights = MESSAGE_PORT_RIGHT_ALL;
    } else if (kind == OS_PORT_COMPLETION) {
        completion_port_t *completion_port = 0;
        status = completion_port_create((uint32_t)capacity, &completion_port);
        port = completion_port;
        rights = COMPLETION_PORT_RIGHT_ALL;
    } else {
        return K_EINVAL;
    }
    if (status != K_OK) return status;
    handle_t handle = 0;
    status = handle_create(&process->handles, port, rights, &handle);
    if (status == K_OK) {
        status = copy_to_user((void __user *)(uintptr_t)output_pointer,
                              &handle, sizeof(handle));
    }
    if (status != K_OK && handle != 0) {
        (void)handle_close(&process->handles, handle);
    }
    object_put(port);
    return status;
}

/* COMPLETION_WAIT(handle, timeout_ns, output_entry)。 */
int64_t syscall_completion_wait(uint64_t handle, uint64_t timeout_ns,
                                   uint64_t output_pointer, uint64_t unused3,
                                   uint64_t unused4, uint64_t unused5) {
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0) return K_EPERM;
    void *object = 0;
    kstatus_t status = handle_lookup(&process->handles, (handle_t)handle,
                                     COMPLETION_PORT_RIGHT_WAIT, &object);
    if (status != K_OK) return status;
    completion_port_t *port = (completion_port_t *)object;
    os_completion_entry_t entry = {0};
    if (port->object.type != KOBJECT_TYPE_COMPLETION_PORT) {
        status = K_EINVAL;
    } else {
        status = completion_port_wait(port, timeout_ns, &entry);
        if (status == K_OK) {
            status = copy_to_user((void __user *)(uintptr_t)output_pointer,
                                  &entry, sizeof(entry));
        }
    }
    object_put(object);
    return status;
}

/* CLOCK_GET(0, output_timespec)：返回单调 TSC 时钟，不受墙上时间校准影响。 */
static atomic_int_fast64_t g_realtime_offset_ns;

static bool clock_timespec_to_ns(const os_timespec_t *value,
                                 int64_t *result) {
    const uint64_t nanoseconds_per_second = 1000000000ULL;
    const uint64_t positive_limit = (uint64_t)INT64_MAX;
    const uint64_t negative_limit = (uint64_t)INT64_MAX + 1ULL;
    uint64_t magnitude;
    uint64_t product;
    uint64_t remainder;

    if (value == 0 || value->nanoseconds < 0 ||
        (uint32_t)value->nanoseconds >= nanoseconds_per_second ||
        result == 0) return false;
    if (value->seconds >= 0) {
        magnitude = (uint64_t)value->seconds;
        if (magnitude > positive_limit / nanoseconds_per_second) return false;
        product = magnitude * nanoseconds_per_second;
        if (product > positive_limit - (uint32_t)value->nanoseconds) return false;
        *result = (int64_t)(product + (uint32_t)value->nanoseconds);
        return true;
    }

    magnitude = (uint64_t)(-(value->seconds + 1)) + 1ULL;
    if (magnitude > negative_limit / nanoseconds_per_second) return false;
    product = magnitude * nanoseconds_per_second;
    remainder = (uint32_t)value->nanoseconds;
    if (product < remainder || product - remainder > negative_limit) return false;
    product -= remainder;
    *result = product == negative_limit ? INT64_MIN : -(int64_t)product;
    return true;
}

static kstatus_t clock_read_value(uint32_t clock_id, os_timespec_t *value) {
    uint64_t monotonic_ns = x86_tsc_to_ns(x86_read_tsc());
    int64_t signed_ns;
    int64_t offset;

    if (clock_id != OS_CLOCK_REALTIME && clock_id != OS_CLOCK_MONOTONIC) {
        return K_EINVAL;
    }
    if (monotonic_ns > (uint64_t)INT64_MAX) return K_EOVERFLOW;
    signed_ns = (int64_t)monotonic_ns;
    if (clock_id == OS_CLOCK_REALTIME) {
        offset = atomic_load_explicit(&g_realtime_offset_ns,
                                      memory_order_acquire);
        if ((offset > 0 && signed_ns > INT64_MAX - offset) ||
            (offset < 0 && signed_ns < INT64_MIN - offset)) {
            return K_EOVERFLOW;
        }
        signed_ns += offset;
    }
    value->seconds = signed_ns / 1000000000LL;
    value->nanoseconds = (int32_t)(signed_ns % 1000000000LL);
    if (value->nanoseconds < 0) {
        --value->seconds;
        value->nanoseconds += 1000000000;
    }
    value->reserved = 0;
    return K_OK;
}

int64_t syscall_clock_get(uint64_t clock_id, uint64_t output_pointer,
                             uint64_t unused2, uint64_t unused3,
                             uint64_t unused4, uint64_t unused5) {
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    os_timespec_t value;
    kstatus_t status = clock_read_value((uint32_t)clock_id, &value);
    if (status != K_OK) return status;
    return copy_to_user((void __user *)(uintptr_t)output_pointer,
                        &value, sizeof(value));
}

int64_t syscall_clock_set(uint64_t arguments_pointer, uint64_t unused1,
                          uint64_t unused2, uint64_t unused3,
                          uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    os_clock_set_t arguments;
    int64_t target_ns;
    uint64_t monotonic_ns;
    int64_t monotonic_signed;
    int64_t offset;

    if (copy_from_user(&arguments,
            (const void __user *)(uintptr_t)arguments_pointer,
            sizeof(arguments)) != K_OK ||
        !versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.clock_id != OS_CLOCK_REALTIME || arguments.reserved != 0U ||
        !clock_timespec_to_ns(&arguments.value, &target_ns)) {
        return K_EINVAL;
    }
    monotonic_ns = x86_tsc_to_ns(x86_read_tsc());
    if (monotonic_ns > (uint64_t)INT64_MAX) return K_EOVERFLOW;
    monotonic_signed = (int64_t)monotonic_ns;
    if ((target_ns >= 0 && monotonic_signed < 0) ||
        (target_ns < 0 && monotonic_signed > 0 &&
         target_ns < INT64_MIN + monotonic_signed)) {
        return K_EOVERFLOW;
    }
    offset = target_ns - monotonic_signed;
    atomic_store_explicit(&g_realtime_offset_ns, offset, memory_order_release);
    return K_OK;
}

/* TIMER_CREATE(delay_ns, period_ns, output_handle)。定时器本身是可等待对象。 */
int64_t syscall_timer_create(uint64_t delay_ns, uint64_t period_ns,
                                uint64_t output_pointer, uint64_t unused3,
                                uint64_t unused4, uint64_t unused5) {
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0) return K_EPERM;
    timer_object_t *timer = 0;
    kstatus_t status = timer_create(delay_ns, period_ns, &timer);
    if (status != K_OK) return status;
    handle_t handle = 0;
    status = handle_create(&process->handles, timer, TIMER_RIGHT_ALL, &handle);
    if (status == K_OK) {
        status = copy_to_user((void __user *)(uintptr_t)output_pointer,
                              &handle, sizeof(handle));
    }
    if (status != K_OK && handle != 0) (void)handle_close(&process->handles, handle);
    object_put(timer);
    return status;
}

/* PORT_SEND(handle, user_buffer, size)。消息端口只承载小型控制消息。 */
int64_t syscall_port_send(uint64_t handle, uint64_t buffer, uint64_t size,
                             uint64_t unused3, uint64_t unused4, uint64_t unused5) {
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0 || size == 0 || size > OS_PORT_MAX_MESSAGE_SIZE) return K_EINVAL;
    void *object = 0;
    kstatus_t status = handle_lookup(&process->handles, (handle_t)handle,
                                     MESSAGE_PORT_RIGHT_WRITE, &object);
    if (status != K_OK) return status;
    uint8_t message[OS_PORT_MAX_MESSAGE_SIZE];
    status = copy_from_user(message, (const void __user *)(uintptr_t)buffer,
                            (size_t)size);
    if (status == K_OK) {
        message_port_t *port = (message_port_t *)object;
        status = port->object.type == KOBJECT_TYPE_MESSAGE_PORT ?
                 message_port_send(port, message, (size_t)size) : K_EINVAL;
    }
    object_put(object);
    return status;
}

/* PORT_RECEIVE(handle, user_buffer, capacity, output_size, timeout_ns)。 */
int64_t syscall_port_receive(uint64_t handle, uint64_t buffer, uint64_t capacity,
                                uint64_t output_size, uint64_t timeout_ns,
                                uint64_t unused5) {
    (void)unused5;
    process_t *process = current_process();
    if (process == 0 || capacity == 0 || capacity > OS_PORT_MAX_MESSAGE_SIZE) {
        return K_EINVAL;
    }
    void *object = 0;
    kstatus_t status = handle_lookup(&process->handles, (handle_t)handle,
                                     MESSAGE_PORT_RIGHT_READ | MESSAGE_PORT_RIGHT_WAIT,
                                     &object);
    if (status != K_OK) return status;
    uint8_t message[OS_PORT_MAX_MESSAGE_SIZE];
    size_t size = 0;
    message_port_t *port = (message_port_t *)object;
    if (port->object.type != KOBJECT_TYPE_MESSAGE_PORT) {
        status = K_EINVAL;
    } else {
        status = message_port_receive(port, message, (size_t)capacity, &size,
                                      timeout_ns);
        if (status == K_OK) {
            status = copy_to_user((void __user *)(uintptr_t)buffer, message, size);
        }
        if (status == K_OK) {
            status = copy_to_user((void __user *)(uintptr_t)output_size,
                                  &size, sizeof(size));
        }
    }
    object_put(object);
    return status;
}

int64_t syscall_wait_one(uint64_t handle, uint64_t timeout_ns,
                            uint64_t output_pointer, uint64_t unused3,
                            uint64_t unused4, uint64_t unused5) {
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0) return K_EPERM;
    liteos_debug_trace_stage(LITEOS_DEBUG_PHASE_USER_RUNTIME,
                             LITEOS_DEBUG_STEP_WAIT_ONE_ENTER,
                             (uint32_t)handle);
    void *object = 0;
    kstatus_t status = handle_lookup(&process->handles, (handle_t)handle,
                                     OBJECT_RIGHT_WAIT, &object);
    if (status != K_OK) {
#if LITEOS_DEBUG_SERIAL
        liteos_serial_write("LITEOS_DIAG_WAIT_ONE_LOOKUP_FAIL STATUS=");
        liteos_serial_write_u32((uint32_t)status);
        liteos_serial_write(" HANDLE_LOW=");
        liteos_serial_write_u32((uint32_t)handle);
        liteos_serial_write(" HANDLE_HIGH=");
        liteos_serial_write_u32((uint32_t)(handle >> 32));
        liteos_serial_write("\r\n");
#endif
        return status;
    }
    void *objects[1] = {object};
    os_wait_result_t result = {
        .index = 0,
        .reserved = 0,
        .value = 0,
    };
    status = object_wait_many(objects, 1U, false, timeout_ns,
                              &result.index, &result.value);
    if (status == K_OK) {
        status = copy_to_user((void __user *)(uintptr_t)output_pointer,
                              &result, sizeof(result));
    }
    liteos_debug_trace_stage(LITEOS_DEBUG_PHASE_USER_RUNTIME,
                             LITEOS_DEBUG_STEP_WAIT_ONE_RETURN,
                             (uint32_t)status);
    object_put(object);
    return status;
}

int64_t syscall_wait_many(uint64_t arguments_pointer, uint64_t unused1,
                             uint64_t unused2, uint64_t unused3,
                             uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0) return K_EPERM;
    os_wait_many_t arguments;
    kstatus_t status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.count == 0 || arguments.count > OS_WAIT_MAX_HANDLES ||
        (arguments.wait_flags & ~OS_WAIT_ALL) != 0 || arguments.reserved != 0 ||
        arguments.handles == 0) return K_EINVAL;

    size_t handle_bytes = (size_t)arguments.count * sizeof(os_handle_t);
    size_t object_bytes = (size_t)arguments.count * sizeof(void *);
    os_handle_t *handles = (os_handle_t *)kmalloc(handle_bytes, 0);
    void **objects = (void **)kmalloc(object_bytes, 0);
    if (handles == 0 || objects == 0) {
        kfree(objects);
        kfree(handles);
        return K_ENOMEM;
    }
    for (uint32_t i = 0; i < arguments.count; ++i) objects[i] = 0;
    status = copy_from_user(handles,
        (const void __user *)(uintptr_t)arguments.handles, handle_bytes);
    uint32_t referenced = 0;
    while (status == K_OK && referenced < arguments.count) {
        status = handle_lookup(&process->handles, handles[referenced],
                               OBJECT_RIGHT_WAIT, &objects[referenced]);
        if (status == K_OK) ++referenced;
    }
    if (status == K_OK) {
        status = object_wait_many(objects, arguments.count,
                                  (arguments.wait_flags & OS_WAIT_ALL) != 0,
                                  arguments.timeout_ns,
                                  &arguments.result_index,
                                  &arguments.result_value);
    }
    if (status == K_OK) {
        status = copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                              &arguments, sizeof(arguments));
    }
    for (uint32_t i = 0; i < referenced; ++i) object_put(objects[i]);
    kfree(objects);
    kfree(handles);
    return status;
}

int64_t syscall_futex_wait(uint64_t address, uint64_t expected, uint64_t timeout_ns,
                              uint64_t flags, uint64_t unused4, uint64_t unused5) {
    (void)unused4;
    (void)unused5;
    if (flags != 0 || expected > UINT32_MAX) return K_EINVAL;
    process_t *process = current_process();
    if (process == 0) return K_EPERM;
    return futex_wait(process, (uint32_t __user *)(uintptr_t)address,
                      (uint32_t)expected, timeout_ns);
}

int64_t syscall_futex_wake(uint64_t address, uint64_t maximum, uint64_t flags,
                              uint64_t unused3, uint64_t unused4, uint64_t unused5) {
    (void)unused3;
    (void)unused4;
    (void)unused5;
    if (flags != 0 || maximum > UINT32_MAX) return K_EINVAL;
    process_t *process = current_process();
    if (process == 0) return K_EPERM;
    uint32_t woken = 0;
    kstatus_t status = futex_wake(process, (uint32_t __user *)(uintptr_t)address,
                                  (uint32_t)maximum, &woken);
    return status == K_OK ? (int64_t)woken : status;
}
