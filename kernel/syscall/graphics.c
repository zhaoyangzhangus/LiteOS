/* REFACTOR_SYSCALL_GRAPHICS_OWNER: GPU, display, input and window handlers. */

#include <arch/x86_64/paging.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/uaccess.h>
#include <kernel/display.h>
#include <kernel/gpu.h>
#include <kernel/input.h>
#include <kernel/mm.h>
#include <kernel/vm.h>
#include <kernel/window_server.h>
#include <uapi/display.h>
#include <uapi/gpu.h>
#include <uapi/input.h>
#include <uapi/mm.h>
#include <uapi/window.h>

#include "internal.h"

static volatile uint32_t g_gpu_submit_progress[MAX_CPUS];

static void gpu_submit_progress(uint32_t value) {
    uint32_t cpu_index = x86_current_cpu_index();
    if (cpu_index < MAX_CPUS) {
        __atomic_store_n(&g_gpu_submit_progress[cpu_index], value,
                         __ATOMIC_RELEASE);
    }
}

uint32_t syscall_gpu_submit_debug_progress(uint32_t cpu_index) {
    if (cpu_index >= MAX_CPUS) return 0U;
    return __atomic_load_n(&g_gpu_submit_progress[cpu_index],
                           __ATOMIC_ACQUIRE);
}

int64_t syscall_gpu_create_context(uint64_t arguments_pointer,
                                      uint64_t output_pointer, uint64_t unused2,
                                      uint64_t unused3, uint64_t unused4,
                                      uint64_t unused5) {
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0) return K_EPERM;
    os_gpu_create_context_t arguments;
    kstatus_t status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.device != OS_INVALID_HANDLE || arguments.flags != 0U) return K_EINVAL;

    gpu_context_t *context = 0;
    status = gpu_context_create(0, process, &context);
    if (status != K_OK) return status;
    handle_t handle = 0;
    status = handle_create(&process->handles, context, GPU_CONTEXT_RIGHT_ALL, &handle);
    if (status == K_OK) {
        status = copy_to_user((void __user *)(uintptr_t)output_pointer,
                              &handle, sizeof(handle));
    }
    if (status != K_OK && handle != 0) (void)handle_close(&process->handles, handle);
    object_put(context);
    return status;
}

/* GPU_ALLOC(args, output_handle)：分配受句柄保护的共享命令缓冲。 */
int64_t syscall_gpu_alloc(uint64_t arguments_pointer, uint64_t output_pointer,
                             uint64_t unused2, uint64_t unused3,
                             uint64_t unused4, uint64_t unused5) {
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0) return K_EPERM;
    os_gpu_alloc_t arguments;
    kstatus_t status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.size == 0U || arguments.flags != 0U || arguments.reserved != 0U) {
        return K_EINVAL;
    }
    gpu_allocation_t *allocation = 0;
    status = gpu_allocation_create(arguments.size, &allocation);
    if (status != K_OK) return status;
    handle_t handle = 0;
    status = handle_create(&process->handles, allocation,
                           GPU_ALLOCATION_RIGHT_ALL, &handle);
    if (status == K_OK) {
        status = copy_to_user((void __user *)(uintptr_t)output_pointer,
                              &handle, sizeof(handle));
    }
    if (status != K_OK && handle != 0) (void)handle_close(&process->handles, handle);
    object_put(allocation);
    return status;
}

/* GPU_SUBMIT(args)：提交进入 deferred 完成队列，由 fence 完成回调 signal。 */
int64_t syscall_gpu_submit(uint64_t arguments_pointer, uint64_t unused1,
                              uint64_t unused2, uint64_t unused3,
                              uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    gpu_submit_progress(1U);
    if (process == 0) return K_EPERM;
    os_gpu_submit_t arguments;
    kstatus_t status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    gpu_submit_progress(2U);
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.context == OS_INVALID_HANDLE ||
        arguments.command_buffer == OS_INVALID_HANDLE ||
        arguments.signal_value == 0U || arguments.command_length == 0U) return K_EINVAL;
    gpu_submit_progress(3U);

    void *context_object = 0;
    status = handle_lookup(&process->handles, (handle_t)arguments.context,
                           GPU_CONTEXT_RIGHT_SUBMIT, &context_object);
    gpu_submit_progress(4U);
    if (status != K_OK) return status;
    void *allocation_object = 0;
    status = handle_lookup(&process->handles, (handle_t)arguments.command_buffer,
                           GPU_ALLOCATION_RIGHT_READ, &allocation_object);
    gpu_submit_progress(5U);
    if (status != K_OK) {
        object_put(context_object);
        return status;
    }
    gpu_fence_t *fence = 0;
    bool created_fence = false;
    handle_t fence_handle = (handle_t)arguments.signal_fence;
    if (fence_handle == OS_INVALID_HANDLE) {
        status = gpu_fence_create(&fence);
        gpu_submit_progress(6U);
        if (status == K_OK) {
            status = handle_create(&process->handles, fence,
                                   GPU_FENCE_RIGHT_ALL, &fence_handle);
            gpu_submit_progress(7U);
            if (status == K_OK) {
                created_fence = true;
                arguments.signal_fence = fence_handle;
            }
        }
    } else {
        void *fence_object = 0;
        status = handle_lookup(&process->handles, fence_handle,
                               GPU_FENCE_RIGHT_SIGNAL, &fence_object);
        gpu_submit_progress(6U);
        if (status == K_OK) fence = (gpu_fence_t *)fence_object;
    }
    if (status == K_OK && (fence == 0 || fence->object.type != KOBJECT_TYPE_GPU_FENCE)) {
        status = K_EINVAL;
    }
    if (status == K_OK) {
        gpu_submit_progress(8U);
        status = gpu_submit((gpu_context_t *)context_object,
                            (gpu_allocation_t *)allocation_object,
                            arguments.command_offset, arguments.command_length,
                            fence, arguments.signal_value);
        gpu_submit_progress(9U);
    }
    if (status == K_OK && created_fence) {
        status = copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                              &arguments, sizeof(arguments));
    }
    if (status != K_OK && created_fence) {
        (void)handle_close(&process->handles, fence_handle);
    }
    if (fence != 0) object_put(fence);
    object_put(allocation_object);
    object_put(context_object);
    gpu_submit_progress(10U);
    return status;
}

/* GPU_WAIT_FENCE(handle, value, timeout_ns)。 */
int64_t syscall_gpu_wait_fence(uint64_t handle, uint64_t value,
                                  uint64_t timeout_ns, uint64_t unused3,
                                  uint64_t unused4, uint64_t unused5) {
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0) return K_EPERM;
    void *object = 0;
    kstatus_t status = handle_lookup(&process->handles, (handle_t)handle,
                                     GPU_FENCE_RIGHT_WAIT, &object);
    if (status != K_OK) return status;
    gpu_fence_t *fence = (gpu_fence_t *)object;
    status = fence->object.type == KOBJECT_TYPE_GPU_FENCE ?
             gpu_fence_wait(fence, value, timeout_ns) : K_EINVAL;
    object_put(object);
    return status;
}

/* GPU_MAP(args)：把分配对象的连续物理页映射到当前进程的用户地址空间。 */
int64_t syscall_gpu_map(uint64_t arguments_pointer, uint64_t unused1,
                           uint64_t unused2, uint64_t unused3,
                           uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0) return K_EPERM;

    os_gpu_map_t arguments;
    kstatus_t status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.allocation == OS_INVALID_HANDLE ||
        (arguments.flags & ~OS_GPU_MAP_FIXED) != 0U ||
        (arguments.prot & ~(OS_VM_READ | OS_VM_WRITE)) != 0U ||
        arguments.prot == 0U || arguments.length == 0U ||
        (arguments.offset & (PAGE_SIZE - 1ULL)) != 0U ||
        (arguments.length & (PAGE_SIZE - 1ULL)) != 0U) return K_EINVAL;

    void *allocation_object = 0;
    uint32_t required_rights = 0U;
    if ((arguments.prot & OS_VM_READ) != 0U) required_rights |= GPU_ALLOCATION_RIGHT_READ;
    if ((arguments.prot & OS_VM_WRITE) != 0U) required_rights |= GPU_ALLOCATION_RIGHT_WRITE;
    status = handle_lookup(&process->handles, (handle_t)arguments.allocation,
                           required_rights, &allocation_object);
    if (status != K_OK) return status;
    gpu_allocation_t *allocation = (gpu_allocation_t *)allocation_object;
    if (allocation->object.type != KOBJECT_TYPE_GPU_ALLOCATION ||
        allocation->backing_page == 0 || arguments.offset > allocation->size ||
        arguments.length > allocation->size - arguments.offset ||
        allocation->backing_phys.value > UINT64_MAX - arguments.offset) {
        object_put(allocation_object);
        return K_EINVAL;
    }

    /* VMA 持有一份 allocation 引用，确保用户映射存在时底层页不会释放。 */
    object_get(allocation);
    vm_object_t *object = 0;
    status = vm_object_create_device(
        paddr_make(allocation->backing_phys.value + arguments.offset),
        arguments.length, X86_CACHE_WB, allocation, object_put, &object);
    if (status != K_OK) {
        object_put(allocation_object);
        object_put(allocation);
        return status;
    }

    vaddr_t address = (vaddr_t)arguments.address;
    uint32_t vm_flags = VM_MAP_SHARED;
    if ((arguments.flags & OS_GPU_MAP_FIXED) != 0U) vm_flags |= VM_MAP_FIXED;
    status = vm_map_object(process->vm, object, &address, 0,
                           (size_t)arguments.length,
                           VM_PROT_USER | translate_vm_protection(arguments.prot),
                           vm_flags);
    vm_object_put(object);
    if (status != K_OK) {
        object_put(allocation_object);
        return status;
    }

    /* 设备页必须在返回用户态前完成映射。这样既避免用户态首次写入时
       触发不可恢复的设备页缺页，也保证返回的地址已经具备有效 PTE。 */
    uint32_t fault_access = translate_vm_protection(arguments.prot);
    for (uint64_t offset = 0; offset < arguments.length; offset += PAGE_SIZE) {
        vm_fault_info_t fault = {
            .address = address + (vaddr_t)offset,
            .access = fault_access,
            .cpu_error = 0,
        };
        status = vm_handle_fault(process->vm, &fault);
        if (status != K_OK) {
            (void)vm_unmap(process->vm, address, (size_t)arguments.length);
            object_put(allocation_object);
            return status;
        }
    }

    arguments.address = (uint64_t)address;
    status = copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                          &arguments, sizeof(arguments));
    if (status != K_OK) {
        (void)vm_unmap(process->vm, address, (size_t)arguments.length);
    }
    object_put(allocation_object);
    return status;
}

/* DISPLAY_COMMIT：提交 GPU 扫描缓冲，并通过 release fence 标记完成。 */
/* 查询当前 GOP 输出的用户态提交参数。 */
int64_t syscall_display_get_info(uint64_t arguments_pointer,
                                    uint64_t unused1, uint64_t unused2,
                                    uint64_t unused3, uint64_t unused4,
                                    uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    os_display_info_t info;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    kstatus_t status = copy_from_user(
        &info, (const void __user *)(uintptr_t)arguments_pointer,
        sizeof(info));
    if (status != K_OK || !versioned_header_valid(&info.hdr, sizeof(info)) ||
        info.output != 0U || info.flags != 0U) return K_EINVAL;
    if (!display_core_query(info.output, &width, &height, &stride, &format)) {
        return K_EIO;
    }
    info.width = width;
    info.height = height;
    info.stride = stride;
    info.format = format;
    return copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                        &info, sizeof(info));
}

/* 从内核输入队列读取一个事件，支持纳秒级超时。 */
int64_t syscall_input_read(uint64_t event_pointer, uint64_t timeout_ns,
                              uint64_t unused2, uint64_t unused3,
                              uint64_t unused4, uint64_t unused5) {
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    input_event_t kernel_event;
    os_input_event_t user_event;
    kstatus_t status;
    if (process == 0 || !window_server_is_manager(process) || event_pointer == 0U) {
        return K_EPERM;
    }
    status = input_core_read(&kernel_event, timeout_ns);
    if (status != K_OK) return status;
    user_event.timestamp = kernel_event.timestamp;
    user_event.device_id = kernel_event.device_id;
    user_event.type = kernel_event.type;
    user_event.flags = kernel_event.flags;
    user_event.code = kernel_event.code;
    user_event.value = kernel_event.value;
    return copy_to_user((void __user *)(uintptr_t)event_pointer,
                        &user_event, sizeof(user_event));
}

static void window_copy_input_event(os_input_event_t *destination,
                                    const input_event_t *source) {
    destination->timestamp = source->timestamp;
    destination->device_id = source->device_id;
    destination->type = source->type;
    destination->flags = source->flags;
    destination->code = source->code;
    destination->value = source->value;
}

int64_t syscall_window_register_manager(uint64_t unused0, uint64_t unused1,
                                           uint64_t unused2, uint64_t unused3,
                                           uint64_t unused4, uint64_t unused5) {
    (void)unused0;
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    return process != 0 ? window_server_register_manager(process) : K_EPERM;
}

int64_t syscall_window_create(uint64_t arguments_pointer, uint64_t unused1,
                                 uint64_t unused2, uint64_t unused3,
                                 uint64_t unused4, uint64_t unused5) {
    process_t *process = current_process();
    os_window_create_t arguments;
    window_server_window_t *window = 0;
    handle_t handle = OS_INVALID_HANDLE;
    kstatus_t status;
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    if (process == 0) return K_EPERM;
    status = copy_from_user(&arguments,
                            (const void __user *)(uintptr_t)arguments_pointer,
                            sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.reserved != 0U ||
        (arguments.flags & ~(OS_WINDOW_VISIBLE | OS_WINDOW_RESIZABLE |
                             OS_WINDOW_CLIENT_DECORATIONS)) != 0U ||
        arguments.width == 0U || arguments.height == 0U) return K_EINVAL;
    status = window_server_create(process, arguments.x, arguments.y,
                                  arguments.width, arguments.height,
                                  arguments.flags, arguments.background,
                                  arguments.title, &window);
    if (status != K_OK) return status;
    status = handle_create(&process->handles, window, WINDOW_RIGHT_ALL, &handle);
    if (status == K_OK) {
        status = window_server_map(window, process, arguments.address,
                                   &arguments.address);
    }
    if (status == K_OK) {
        status = window_server_set_owner_address(window, arguments.address);
    }
    if (status == K_OK) {
        arguments.window = handle;
        arguments.identifier = window_server_window_identifier(window);
        arguments.buffer_size = window_server_window_buffer_size(window);
        status = copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                              &arguments, sizeof(arguments));
    }
    if (status != K_OK && handle != OS_INVALID_HANDLE) {
        (void)handle_close(&process->handles, handle);
    }
    object_put(window);
    return status;
}

/* WINDOW_UPDATE：客户完成自己的 surface 绘制后请求内核合成并显示。 */
int64_t syscall_window_update(uint64_t arguments_pointer, uint64_t unused1,
                                 uint64_t unused2, uint64_t unused3,
                                 uint64_t unused4, uint64_t unused5) {
    process_t *process = current_process();
    os_window_update_t arguments;
    kstatus_t status;
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    if (process == 0 || arguments_pointer == 0U) return K_EPERM;
    status = copy_from_user(&arguments,
                            (const void __user *)(uintptr_t)arguments_pointer,
                            sizeof(arguments));
    if (status != K_OK || !versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.identifier == 0U || arguments.flags != 0U ||
        ((arguments.width == 0U) != (arguments.height == 0U))) return K_EINVAL;
    return window_server_update(process, arguments.identifier, arguments.x,
                                arguments.y, arguments.width, arguments.height,
                                arguments.flags);
}

int64_t syscall_window_enumerate(uint64_t arguments_pointer, uint64_t unused1,
                                    uint64_t unused2, uint64_t unused3,
                                    uint64_t unused4, uint64_t unused5) {
    process_t *process = current_process();
    os_window_enumerate_t arguments;
    window_server_snapshot_t snapshot;
    kstatus_t status;
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    if (process == 0 || !window_server_is_manager(process)) return K_EPERM;
    status = copy_from_user(&arguments,
                            (const void __user *)(uintptr_t)arguments_pointer,
                            sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.reserved != 0U) return K_EINVAL;
    status = window_server_snapshot(arguments.index, &snapshot);
    if (status != K_OK) return status;
    arguments.info.identifier = snapshot.identifier;
    arguments.info.owner_pid = snapshot.owner_pid;
    arguments.info.x = snapshot.x;
    arguments.info.y = snapshot.y;
    arguments.info.width = snapshot.width;
    arguments.info.height = snapshot.height;
    arguments.info.visible = snapshot.visible;
    arguments.info.focused = snapshot.focused;
    arguments.info.z_order = snapshot.z_order;
    arguments.info.reserved = 0U;
    arguments.info.buffer_size = snapshot.buffer_size;
    for (uint32_t i = 0U; i < sizeof(arguments.info.title); ++i) {
        arguments.info.title[i] = snapshot.title[i];
    }
    return copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                        &arguments, sizeof(arguments));
}

int64_t syscall_window_map(uint64_t arguments_pointer, uint64_t unused1,
                              uint64_t unused2, uint64_t unused3,
                              uint64_t unused4, uint64_t unused5) {
    process_t *process = current_process();
    os_window_map_t arguments;
    window_server_window_t *window = 0;
    kstatus_t status;
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    if (process == 0 || !window_server_is_manager(process)) return K_EPERM;
    status = copy_from_user(&arguments,
                            (const void __user *)(uintptr_t)arguments_pointer,
                            sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.reserved != 0U) return K_EINVAL;
    status = window_server_lookup(arguments.identifier, &window);
    if (status != K_OK) return status;
    if (arguments.length != 0U &&
        arguments.length != window_server_window_buffer_size(window)) {
        window_server_put(window);
        return K_EINVAL;
    }
    status = window_server_map(window, process, arguments.address,
                               &arguments.address);
    arguments.length = window_server_window_buffer_size(window);
    window_server_put(window);
    if (status != K_OK) return status;
    return copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                        &arguments, sizeof(arguments));
}

int64_t syscall_window_set(uint64_t arguments_pointer, uint64_t unused1,
                              uint64_t unused2, uint64_t unused3,
                              uint64_t unused4, uint64_t unused5) {
    process_t *process = current_process();
    os_window_set_t arguments;
    window_server_window_t *window = 0;
    kstatus_t status;
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    if (process == 0 || !window_server_is_manager(process)) return K_EPERM;
    status = copy_from_user(&arguments,
                            (const void __user *)(uintptr_t)arguments_pointer,
                            sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.reserved != 0U || arguments.visible > 1U) return K_EINVAL;
    status = window_server_lookup(arguments.identifier, &window);
    if (status == K_OK) status = window_server_set(window, arguments.x,
                                                   arguments.y,
                                                   arguments.visible);
    window_server_put(window);
    return status;
}

int64_t syscall_window_focus(uint64_t arguments_pointer, uint64_t unused1,
                                uint64_t unused2, uint64_t unused3,
                                uint64_t unused4, uint64_t unused5) {
    process_t *process = current_process();
    os_window_focus_t arguments;
    kstatus_t status;
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    if (process == 0 || !window_server_is_manager(process)) return K_EPERM;
    status = copy_from_user(&arguments,
                            (const void __user *)(uintptr_t)arguments_pointer,
                            sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.reserved != 0U) return K_EINVAL;
    return window_server_focus(arguments.identifier);
}

int64_t syscall_window_input_read(uint64_t event_pointer, uint64_t timeout_ns,
                                     uint64_t unused2, uint64_t unused3,
                                     uint64_t unused4, uint64_t unused5) {
    process_t *process = current_process();
    input_event_t kernel_event;
    os_input_event_t user_event;
    kstatus_t status;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    if (process == 0 || !window_server_is_manager(process) || event_pointer == 0U) {
        return K_EPERM;
    }
    status = input_core_read(&kernel_event, timeout_ns);
    if (status != K_OK) return status;
    window_copy_input_event(&user_event, &kernel_event);
    return copy_to_user((void __user *)(uintptr_t)event_pointer,
                        &user_event, sizeof(user_event));
}

int64_t syscall_window_input_dispatch(uint64_t arguments_pointer,
                                         uint64_t unused1, uint64_t unused2,
                                         uint64_t unused3, uint64_t unused4,
                                         uint64_t unused5) {
    process_t *process = current_process();
    os_window_input_dispatch_t arguments;
    kstatus_t status;
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    if (process == 0 || !window_server_is_manager(process)) return K_EPERM;
    status = copy_from_user(&arguments,
                            (const void __user *)(uintptr_t)arguments_pointer,
                            sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.reserved != 0U) return K_EINVAL;
    return window_server_dispatch(arguments.identifier, &arguments.event);
}

int64_t syscall_window_event_read(uint64_t arguments_pointer, uint64_t unused1,
                                     uint64_t unused2, uint64_t unused3,
                                     uint64_t unused4, uint64_t unused5) {
    process_t *process = current_process();
    os_window_event_read_t arguments;
    kstatus_t status;
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    if (process == 0) return K_EPERM;
    status = copy_from_user(&arguments,
                            (const void __user *)(uintptr_t)arguments_pointer,
                            sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.reserved != 0U) return K_EINVAL;
    status = window_server_event_read(process, arguments.identifier,
                                      &arguments.event, arguments.timeout_ns);
    if (status != K_OK) return status;
    return copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                        &arguments, sizeof(arguments));
}

int64_t syscall_display_commit(uint64_t arguments_pointer,
                                  uint64_t unused1, uint64_t unused2,
                                  uint64_t unused3, uint64_t unused4,
                                  uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    os_display_commit_t arguments;
    gpu_allocation_t *buffer = 0;
    gpu_fence_t *wait_fence = 0;
    gpu_fence_t *signal_fence = 0;
    handle_t signal_handle = 0;
    bool created_signal = false;
    kstatus_t status;
    if (process == 0) return K_EPERM;
    status = copy_from_user(&arguments,
                            (const void __user *)(uintptr_t)arguments_pointer,
                            sizeof(arguments));
    if (status != K_OK || !versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.output != 0U || arguments.flags != 0U || arguments.reserved != 0U ||
        arguments.buffer == OS_INVALID_HANDLE || arguments.width == 0U ||
        arguments.height == 0U || arguments.stride == 0U || arguments.format != 1U ||
        arguments.signal_value == 0U ||
        (arguments.wait_fence == OS_INVALID_HANDLE && arguments.wait_value != 0U) ||
        (arguments.wait_fence != OS_INVALID_HANDLE && arguments.wait_value == 0U) ||
        (arguments.signal_fence == arguments.wait_fence &&
         arguments.signal_fence != OS_INVALID_HANDLE)) return K_EINVAL;

    status = handle_lookup(&process->handles, (handle_t)arguments.buffer,
                           GPU_ALLOCATION_RIGHT_READ, (void **)&buffer);
    if (status != K_OK) return status;
    if (buffer->object.type != KOBJECT_TYPE_GPU_ALLOCATION) {
        status = K_EINVAL;
        goto display_commit_cleanup;
    }
    if (arguments.wait_fence != OS_INVALID_HANDLE) {
        void *object = 0;
        status = handle_lookup(&process->handles, (handle_t)arguments.wait_fence,
                               GPU_FENCE_RIGHT_WAIT, &object);
        if (status != K_OK) goto display_commit_cleanup;
        wait_fence = (gpu_fence_t *)object;
        if (wait_fence->object.type != KOBJECT_TYPE_GPU_FENCE) {
            status = K_EINVAL;
            goto display_commit_cleanup;
        }
    }
    if (arguments.signal_fence == OS_INVALID_HANDLE) {
        status = gpu_fence_create(&signal_fence);
        if (status != K_OK) goto display_commit_cleanup;
        status = handle_create(&process->handles, signal_fence,
                               GPU_FENCE_RIGHT_ALL, &signal_handle);
        if (status != K_OK) goto display_commit_cleanup;
        created_signal = true;
    } else {
        void *object = 0;
        status = handle_lookup(&process->handles, (handle_t)arguments.signal_fence,
                               GPU_FENCE_RIGHT_SIGNAL, &object);
        if (status != K_OK) goto display_commit_cleanup;
        signal_fence = (gpu_fence_t *)object;
        signal_handle = (handle_t)arguments.signal_fence;
        if (signal_fence->object.type != KOBJECT_TYPE_GPU_FENCE) {
            status = K_EINVAL;
            goto display_commit_cleanup;
        }
    }
    status = display_commit_submit(arguments.output, buffer, arguments.offset,
                                   arguments.stride, arguments.width,
                                   arguments.height, arguments.format,
                                   wait_fence, arguments.wait_value,
                                   signal_fence, arguments.signal_value);
    if (status == K_OK && created_signal) {
        arguments.signal_fence = signal_handle;
        status = copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                              &arguments, sizeof(arguments));
    }
    if (status != K_OK && created_signal && signal_handle != 0) {
        (void)handle_close(&process->handles, signal_handle);
    }

display_commit_cleanup:
    if (signal_fence != 0) object_put(signal_fence);
    if (wait_fence != 0) object_put(wait_fence);
    if (buffer != 0) object_put(buffer);
    return status;
}

