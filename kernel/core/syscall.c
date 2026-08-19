#include "syscall.h"
#include <arch/x86_64/paging.h>
#include <arch/x86_64/uaccess.h>
#include <kernel/futex.h>
#include <kernel/console.h>
#include <kernel/completion_port.h>
#include <kernel/message_port.h>
#include <kernel/timer.h>
#include <kernel/deferred.h>
#include <kernel/mm.h>
#include <kernel/kmem.h>
#include <kernel/vm.h>
#include <kernel/shared_section.h>
#include <kernel/process.h>
#include <kernel/security.h>
#include <kernel/vfs.h>
#include <kernel/socket.h>
#include <kernel/net_manager.h>
#include <kernel/e1000.h>
#include <kernel/device.h>
#include <kernel/io.h>
#include <kernel/audio.h>
#include <kernel/hda.h>
#include <kernel/xhci.h>
#include <kernel/gpu.h>
#include <kernel/display.h>
#include <kernel/input.h>
#include <kernel/window_server.h>
#include <uapi/mm.h>
#include <uapi/io.h>
#include <uapi/ipc.h>
#include <uapi/process.h>
#include <uapi/socket.h>
#include <uapi/device.h>
#include <uapi/file.h>
#include <uapi/audio.h>
#include <uapi/wait.h>
#include <uapi/gpu.h>
#include <uapi/display.h>
#include <uapi/input.h>
#include <uapi/window.h>
#include <uapi/network.h>
#include <uapi/security.h>

static volatile uint32_t g_thread_create_stage;

static int64_t thread_create_diag_fail(uint32_t step, kstatus_t status) {
    liteos_serial_write("LITEOS_DIAG_THREAD_CREATE_FAIL STEP=");
    liteos_serial_write_u32(step);
    liteos_serial_write(" STATUS=");
    liteos_serial_write_u32((uint32_t)status);
    liteos_serial_write(" PROCESS_STAGE=");
    liteos_serial_write_u32(process_last_thread_create_stage());
    liteos_serial_write(" VMALLOC_FAIL=");
    liteos_serial_write_u32(vmalloc_last_failure());
    liteos_serial_write("\r\n");
    return status;
}


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

#define IA32_EFER               0xC0000080U
#define IA32_STAR               0xC0000081U
#define IA32_LSTAR              0xC0000082U
#define IA32_FMASK              0xC0000084U
#define IA32_GS_BASE            0xC0000101U
#define IA32_KERNEL_GS_BASE     0xC0000102U

#define EFER_SYSCALL_ENABLE     (1ULL << 0)
#define RFLAGS_CARRY            (1ULL << 0)
#define RFLAGS_FIXED            (1ULL << 1)
#define RFLAGS_TRAP             (1ULL << 8)
#define RFLAGS_INTERRUPT        (1ULL << 9)
#define RFLAGS_DIRECTION        (1ULL << 10)
#define RFLAGS_IOPL             (3ULL << 12)
#define RFLAGS_NESTED_TASK      (1ULL << 14)
#define RFLAGS_RESUME           (1ULL << 16)
#define RFLAGS_VIRTUAL_8086     (1ULL << 17)
#define RFLAGS_ALIGNMENT_CHECK  (1ULL << 18)
#define RFLAGS_ID                (1ULL << 21)

/*
 * RFLAGS 中未定义位不能带入 SYSRETQ/IRETQ。
 *
 * 用户态可以保留算术标志、TF/IF/DF、RF、AC 和 ID；IOPL、NT、VM、VIF、VIP
 * 等位必须由返回路径拒绝。其余保留位若被伪造，不能让处理器在返回时解释
 * 出未定义状态。
 */
#define RFLAGS_USER_ALLOWED     ((1ULL << 0) | (1ULL << 1) | (1ULL << 2) | \
                                 (1ULL << 4) | (1ULL << 6) | (1ULL << 7) | \
                                 RFLAGS_TRAP | RFLAGS_INTERRUPT | \
                                 RFLAGS_DIRECTION | (1ULL << 11) | \
                                 RFLAGS_RESUME | RFLAGS_ALIGNMENT_CHECK | \
                                 RFLAGS_ID)

#define USER_ADDRESS_MIN        0x0000000000010000ULL
#define USER_ADDRESS_END        0x0000800000000000ULL
#define USER_CODE_SELECTOR      0x23ULL
#define USER_DATA_SELECTOR      0x1BULL
#define SYSCALL_TABLE_SIZE      (OS_SYS_WINDOW_UPDATE + 1U)

typedef int64_t (*syscall_handler_t)(uint64_t, uint64_t, uint64_t,
                                     uint64_t, uint64_t, uint64_t);

LITEOS_SYSCALL_CPU_LOCAL liteos_syscall_cpu_local;

static uint64_t read_msr(uint32_t index) {
    uint32_t low;
    uint32_t high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(index));
    return ((uint64_t)high << 32) | low;
}

static void write_msr(uint32_t index, uint64_t value) {
    __asm__ volatile ("wrmsr" : : "a"((uint32_t)value), "d"((uint32_t)(value >> 32)),
                      "c"(index) : "memory");
}

static bool user_address_valid(uint64_t address) {
    return address >= USER_ADDRESS_MIN && address < USER_ADDRESS_END;
}

static bool versioned_header_valid(const os_versioned_header_t *header,
                                   size_t required_size) {
    return header != 0 && header->size >= required_size &&
           header->version == OS_SYSCALL_ABI_VERSION && header->flags == 0;
}

static process_t *current_process(void) {
    thread_t *thread = sched_current_thread();
    if (thread == 0 || thread->object.type != KOBJECT_TYPE_THREAD ||
        thread->process == 0) return 0;
    return thread->process;
}

/* 句柄 0 表示当前进程；非零句柄必须拥有调用所需的进程权限。 */
static kstatus_t lookup_process(process_t *caller, handle_t handle, uint32_t rights,
                                process_t **out, bool *referenced) {
    if (caller == 0 || out == 0 || referenced == 0) return K_EINVAL;
    if (handle == OS_INVALID_HANDLE) {
        *out = caller;
        *referenced = false;
        return K_OK;
    }
    void *object = 0;
    kstatus_t status = handle_lookup(&caller->handles, handle, rights, &object);
    if (status != K_OK) return status;
    object_header_t *header = (object_header_t *)object;
    if (header->type != KOBJECT_TYPE_PROCESS) {
        object_put(object);
        return K_EINVAL;
    }
    *out = (process_t *)object;
    *referenced = true;
    return K_OK;
}

static int64_t sys_thread_exit(uint64_t status, uint64_t unused1, uint64_t unused2,
                               uint64_t unused3, uint64_t unused4, uint64_t unused5) {
    (void)status;
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    LITEOS_SYSCALL_CPU_LOCAL *local = x86_cpu_local_current();
    if (local == 0) return K_EIO;
    local->UserExitSeen = 1U;
    if (local->KernelResumeStack != 0) {
        local->ReturnToKernel = 1U;
        return K_OK;
    }
    if (current_process() == 0) return K_EPERM;
    thread_exit((int64_t)status);
}

static int64_t sys_process_exit(uint64_t status, uint64_t unused1, uint64_t unused2,
                                uint64_t unused3, uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    if (current_process() == 0) return K_EPERM;
    process_exit((int64_t)status);
    return K_OK;
}

static int64_t sys_process_exec(uint64_t path, uint64_t argv, uint64_t envp,
                                uint64_t unused3, uint64_t unused4, uint64_t unused5) {
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0) return K_EPERM;
    return process_exec(process, (const char __user *)(uintptr_t)path,
                        (const char __user *const __user *)(uintptr_t)argv,
                        (const char __user *const __user *)(uintptr_t)envp);
}

/*
 * THREAD_CREATE(target_process, args, out_handle)：target_process 为 0 时创建
 * 当前进程线程。先写回句柄，最后才把线程发布到运行队列，失败路径不会留下孤儿线程。
 */
static int64_t sys_thread_create(uint64_t process_handle, uint64_t arguments_pointer,
                                 uint64_t output_pointer, uint64_t unused3,
                                 uint64_t unused4, uint64_t unused5) {
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *caller = current_process();
    g_thread_create_stage = 1U;
    if (caller == 0) {
        return thread_create_diag_fail(1U, K_EPERM);
    }
    os_thread_create_t arguments;
    kstatus_t status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    if (status != K_OK) {
        return thread_create_diag_fail(2U, status);
    }
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.flags != 0 || arguments.reserved != 0) {
        return thread_create_diag_fail(3U, K_EINVAL);
    }

    process_t *target = 0;
    bool target_referenced = false;
    status = lookup_process(caller, (handle_t)process_handle,
                            PROCESS_RIGHT_CREATE_THREAD, &target, &target_referenced);
    g_thread_create_stage = 2U;
    if (status != K_OK) {
        return thread_create_diag_fail(4U, status);
    }

    /* 创建前同时验证入口和栈权限；按需匿名页会在这里完成首次缺页。 */
    vm_fault_info_t entry_fault = {
        .address = (vaddr_t)arguments.entry,
        .access = VM_PROT_EXEC,
        .cpu_error = 0,
    };
    vm_fault_info_t stack_fault = {
        .address = arguments.stack_top >= sizeof(uint64_t) ?
                   (vaddr_t)(arguments.stack_top - sizeof(uint64_t)) : 0,
        .access = VM_PROT_WRITE,
        .cpu_error = 0,
    };
    status = vm_handle_fault(target->vm, &entry_fault);
    g_thread_create_stage = 3U;
    if (status != K_OK) {
        if (target_referenced) object_put(target);
        return thread_create_diag_fail(5U, status);
    }

    status = vm_handle_fault(target->vm, &stack_fault);
    g_thread_create_stage = 4U;
    if (status != K_OK) {
        if (target_referenced) object_put(target);
        return thread_create_diag_fail(6U, status);
    }

    thread_t *thread = 0;
    handle_t handle = 0;
    status = thread_create_user_suspended(target, (vaddr_t)arguments.entry,
                                          (vaddr_t)arguments.stack_top,
                                          (vaddr_t)arguments.fs_base,
                                          arguments.argument, &thread);
    g_thread_create_stage = 5U;
    if (target_referenced) object_put(target);
    if (status != K_OK) {
        return thread_create_diag_fail(7U, status);
    }
    status = handle_create(&caller->handles, thread, THREAD_RIGHT_ALL, &handle);
    g_thread_create_stage = 6U;
    uint32_t failure_step = 8U;
    if (status == K_OK) {
        failure_step = 9U;
        status = copy_to_user((void __user *)(uintptr_t)output_pointer,
                              &handle, sizeof(handle));
    }
    if (status == K_OK) {
        failure_step = 10U;
        status = thread_start(thread);
        g_thread_create_stage = 7U;
    }
    if (status != K_OK) {
        (void)thread_create_diag_fail(failure_step, status);
    }
    if (status != K_OK && handle != 0) {
        (void)handle_close(&caller->handles, handle);
        handle = 0;
        /* 写回已成功但发布失败时，尽力把用户可见句柄清零。 */
        (void)copy_to_user((void __user *)(uintptr_t)output_pointer,
                           &handle, sizeof(handle));
    }
    if (status != K_OK) (void)thread_terminate(thread, K_ECANCELED);
    object_put(thread);
    return status;
}

/* PROCESS_CREATE(flags, out_handle) 当前提供带 COW 地址空间的空子进程。 */
static int64_t sys_process_create(uint64_t flags, uint64_t output_pointer,
                                  uint64_t unused2, uint64_t unused3,
                                  uint64_t unused4, uint64_t unused5) {
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    if (flags != 0) return K_EINVAL;
    process_t *caller = current_process();
    if (caller == 0) return K_EPERM;
    process_t *child = 0;
    kstatus_t status = process_create(caller, &child);
    if (status != K_OK) return status;
    handle_t handle = 0;
    status = handle_create(&caller->handles, child, PROCESS_RIGHT_ALL, &handle);
    if (status == K_OK) {
        status = copy_to_user((void __user *)(uintptr_t)output_pointer,
                              &handle, sizeof(handle));
    }
    if (status != K_OK && handle != 0) (void)handle_close(&caller->handles, handle);
    object_put(child);
    return status;
}

static uint32_t translate_vm_protection(uint32_t protection) {
    uint32_t result = VM_PROT_USER;
    if ((protection & OS_VM_READ) != 0) result |= VM_PROT_READ;
    if ((protection & OS_VM_WRITE) != 0) result |= VM_PROT_WRITE;
    if ((protection & OS_VM_EXEC) != 0) result |= VM_PROT_EXEC;
    return result;
}

static uint32_t translate_vm_flags(uint32_t flags) {
    uint32_t result = 0;
    if ((flags & OS_VM_PRIVATE) != 0) result |= VM_MAP_PRIVATE;
    if ((flags & OS_VM_SHARED) != 0) result |= VM_MAP_SHARED;
    if ((flags & OS_VM_FIXED) != 0) result |= VM_MAP_FIXED;
    if ((flags & OS_VM_STACK) != 0) result |= VM_MAP_STACK;
    return result;
}

/* VM_MAP(args) 在同一版本化结构的 address 字段中写回最终地址。 */
static int64_t sys_vm_map(uint64_t arguments_pointer, uint64_t unused1,
                          uint64_t unused2, uint64_t unused3,
                          uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0) return K_EPERM;
    os_vm_map_args_t arguments;
    kstatus_t status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    if (status != K_OK) return status;
    const uint32_t valid_protection = OS_VM_READ | OS_VM_WRITE | OS_VM_EXEC;
    const uint32_t valid_flags = OS_VM_PRIVATE | OS_VM_SHARED | OS_VM_FIXED | OS_VM_STACK;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        (arguments.prot & ~valid_protection) != 0 ||
        (arguments.flags & ~valid_flags) != 0 ||
        ((arguments.flags & OS_VM_PRIVATE) != 0 &&
         (arguments.flags & OS_VM_SHARED) != 0) ||
        ((arguments.flags & OS_VM_STACK) != 0 &&
         (arguments.flags & OS_VM_SHARED) != 0) ||
        (arguments.length & (PAGE_SIZE - 1ULL)) != 0 ||
        (arguments.offset & (PAGE_SIZE - 1ULL)) != 0) return K_EINVAL;

    vm_object_t *mapped_object = 0;
    uint64_t mapped_offset = 0;
    void *file_object = 0;
    shared_section_t *shared_section = 0;
    if (arguments.object != OS_INVALID_HANDLE) {
        uint32_t rights = FILE_RIGHT_READ;
        if (handle_lookup(&process->handles, (handle_t)arguments.object,
                          SHARED_SECTION_RIGHT_MAP, (void **)&shared_section) == K_OK &&
            shared_section->object.type == KOBJECT_TYPE_SHARED_SECTION) {
            if ((arguments.flags & OS_VM_SHARED) == 0 ||
                arguments.offset > shared_section->size ||
                arguments.length > shared_section->size - arguments.offset) {
                object_put(shared_section);
                return K_EINVAL;
            }
            mapped_object = shared_section->vm_object;
            mapped_offset = arguments.offset;
            vm_object_get(mapped_object);
            object_put(shared_section);
            shared_section = 0;
            goto map_object;
        }
        if (shared_section != 0) {
            object_put(shared_section);
            shared_section = 0;
        }
        /* 私有写映射只读取文件页；只有共享写映射才要求文件写权限。 */
        if ((arguments.prot & OS_VM_WRITE) != 0 &&
            (arguments.flags & OS_VM_SHARED) != 0) rights |= FILE_RIGHT_WRITE;
        status = handle_lookup(&process->handles, (handle_t)arguments.object,
                               rights, &file_object);
        if (status != K_OK) return status;
        file_t *file = (file_t *)file_object;
        if (file->object.type != KOBJECT_TYPE_FILE || file->vnode == 0 ||
            arguments.offset >= file->vnode->size) {
            object_put(file_object);
            return K_EINVAL;
        }
        status = vm_object_create_file(file->vnode, arguments.offset,
                                       (size_t)arguments.length, &mapped_object);
        object_put(file_object);
        if (status != K_OK) return status;
    } else if (arguments.offset != 0U) {
        return K_EINVAL;
    }

map_object:
    vaddr_t address = (vaddr_t)arguments.address;
    status = vm_map_object(process->vm, mapped_object, &address, mapped_offset,
                           (size_t)arguments.length,
                           translate_vm_protection(arguments.prot),
                           translate_vm_flags(arguments.flags));
    if (mapped_object != 0) vm_object_put(mapped_object);
    if (status != K_OK) return status;
    arguments.address = (uint64_t)address;
    status = copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                          &arguments, sizeof(arguments));
    if (status != K_OK) {
        /* 用户态看不到返回地址时，映射也不能留在进程中。 */
        (void)vm_unmap(process->vm, address, (size_t)arguments.length);
    }
    return status;
}

/* VM_SHARE 创建由句柄持有的匿名共享段，实际页按缺页时分配。 */
static int64_t sys_vm_share(uint64_t arguments_pointer, uint64_t unused1,
                            uint64_t unused2, uint64_t unused3,
                            uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0) return K_EPERM;
    os_vm_share_args_t arguments;
    kstatus_t status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.flags != 0U || arguments.reserved != 0U ||
        arguments.size == 0U || arguments.size > (uint64_t)SIZE_MAX ||
        (arguments.size & (PAGE_SIZE - 1ULL)) != 0U) return K_EINVAL;

    shared_section_t *section = 0;
    status = shared_section_create(arguments.size, &section);
    if (status != K_OK) return status;
    handle_t handle = OS_INVALID_HANDLE;
    status = handle_create(&process->handles, section,
                           SHARED_SECTION_RIGHT_ALL, &handle);
    if (status == K_OK) {
        arguments.section = handle;
        status = copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                              &arguments, sizeof(arguments));
    }
    if (status != K_OK && handle != OS_INVALID_HANDLE) {
        (void)handle_close(&process->handles, handle);
    }
    object_put(section);
    return status;
}

static int64_t sys_vm_unmap(uint64_t address, uint64_t length, uint64_t unused2,
                            uint64_t unused3, uint64_t unused4, uint64_t unused5) {
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    return process != 0 ? vm_unmap(process->vm, (vaddr_t)address, (size_t)length) : K_EPERM;
}

static int64_t sys_vm_protect(uint64_t address, uint64_t length, uint64_t protection,
                              uint64_t unused3, uint64_t unused4, uint64_t unused5) {
    (void)unused3;
    (void)unused4;
    (void)unused5;
    const uint32_t valid = OS_VM_READ | OS_VM_WRITE | OS_VM_EXEC;
    if ((protection & ~valid) != 0) return K_EINVAL;
    process_t *process = current_process();
    return process != 0 ? vm_protect(process->vm, (vaddr_t)address, (size_t)length,
                                     translate_vm_protection((uint32_t)protection)) : K_EPERM;
}

static int64_t sys_handle_close(uint64_t handle, uint64_t unused1, uint64_t unused2,
                                uint64_t unused3, uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    int64_t status = process != 0 ? handle_close(&process->handles, (handle_t)handle) : K_EPERM;
    return status;
}

static kstatus_t audio_uapi_to_kernel(const os_audio_stream_config_t *source,
                                      audio_format_t *destination) {
    if (source == 0 || destination == 0 ||
        !versioned_header_valid(&source->hdr, sizeof(*source)) ||
        source->direction > OS_AUDIO_CAPTURE) return K_EINVAL;
    destination->sample_rate = source->sample_rate;
    destination->channels = source->channels;
    destination->sample_format = source->sample_format;
    destination->period_frames = source->period_frames;
    destination->period_count = source->period_count;
    return K_OK;
}

static uint32_t audio_uapi_state(audio_stream_state_t state) {
    if (state == AUDIO_STREAM_RUNNING) return OS_AUDIO_STATE_RUNNING;
    if (state == AUDIO_STREAM_DISCONNECTED) return OS_AUDIO_STATE_DISCONNECTED;
    return OS_AUDIO_STATE_READY;
}

/* 音频对象只暴露统一 UAPI，实际控制交给存在的硬件后端。 */
static kstatus_t audio_backend_configure(audio_stream_t *stream) {
    kstatus_t status = hda_audio_stream_configure(stream);
    return status == K_ENOENT ? xhci_audio_stream_configure(stream) : status;
}

static kstatus_t audio_backend_start(audio_stream_t *stream) {
    kstatus_t status = hda_audio_stream_start(stream);
    return status == K_ENOENT ? xhci_audio_stream_start(stream) : status;
}

static kstatus_t audio_backend_stop(audio_stream_t *stream) {
    kstatus_t status = hda_audio_stream_stop(stream);
    return status == K_ENOENT ? xhci_audio_stream_stop(stream) : status;
}

static kstatus_t audio_backend_queue(audio_stream_t *stream, uint32_t period,
                                     uint32_t frames) {
    kstatus_t status = xhci_audio_stream_queue(stream, period, frames);
    return status == K_ENOENT ? K_ENOENT : status;
}

static kstatus_t audio_backend_reset(audio_stream_t *stream) {
    kstatus_t status = hda_audio_stream_reset(stream);
    return status == K_ENOENT ? xhci_audio_stream_reset(stream) : status;
}

static kstatus_t audio_backend_disconnect(audio_stream_t *stream) {
    kstatus_t status = hda_audio_stream_disconnect(stream);
    return status == K_ENOENT ? xhci_audio_stream_disconnect(stream) : status;
}

/* AUDIO_OPEN(args)：打开当前可用的 HDA 或 USB Audio DMA 流。 */
static int64_t sys_audio_open(uint64_t arguments_pointer, uint64_t unused1,
                              uint64_t unused2, uint64_t unused3,
                              uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    audio_stream_t *stream = 0;
    audio_format_t format;
    device_t *device;
    handle_t handle = OS_INVALID_HANDLE;
    os_audio_open_t arguments;
    kstatus_t status;
    if (process == 0) return K_EPERM;
    status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments))) return K_EINVAL;
    status = audio_uapi_to_kernel(&arguments.config, &format);
    if (status != K_OK) return status;
    device = hda_audio_device();
    if (device == 0) device = xhci_audio_device();
    if (device == 0 || atomic_load_explicit(&device->state, memory_order_acquire) >=
                       DEVICE_REMOVING) return K_ENOENT;
    status = audio_stream_create(device,
                                 (audio_direction_t)arguments.config.direction,
                                 &stream);
    if (status != K_OK) return status;
    status = audio_stream_configure(stream, &format);
    if (status == K_OK) status = audio_backend_configure(stream);
    if (status == K_OK) {
        status = handle_create(&process->handles, stream,
                               AUDIO_STREAM_RIGHT_ALL, &handle);
    }
    if (status == K_OK) {
        arguments.handle = handle;
        status = copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                              &arguments, sizeof(arguments));
    }
    if (status != K_OK && handle != OS_INVALID_HANDLE) {
        (void)handle_close(&process->handles, handle);
    }
    audio_stream_destroy(stream);
    return status;
}

/* AUDIO_CONTROL(handle,args)：控制流状态，并负责周期数据的用户拷贝。 */
static int64_t sys_audio_control(uint64_t handle, uint64_t arguments_pointer,
                                 uint64_t unused2, uint64_t unused3,
                                 uint64_t unused4, uint64_t unused5) {
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    os_audio_control_t arguments;
    audio_stream_t *stream;
    void *object = 0;
    void *temporary = 0;
    kstatus_t status;
    uint32_t rights;
    uint64_t expected;
    if (process == 0) return K_EPERM;
    status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.flags != 0U || arguments.buffer_size > (uint64_t)SIZE_MAX) {
        return K_EINVAL;
    }
    switch (arguments.code) {
    case OS_AUDIO_CONTROL_CONFIGURE:
    case OS_AUDIO_CONTROL_START:
    case OS_AUDIO_CONTROL_STOP:
    case OS_AUDIO_CONTROL_QUEUE:
    case OS_AUDIO_CONTROL_COMPLETE:
    case OS_AUDIO_CONTROL_RECOVER:
    case OS_AUDIO_CONTROL_RESET:
    case OS_AUDIO_CONTROL_DISCONNECT:
        rights = AUDIO_STREAM_RIGHT_CONTROL;
        break;
    case OS_AUDIO_CONTROL_GET_STATS:
        rights = AUDIO_STREAM_RIGHT_QUERY;
        break;
    default:
        return K_ENOSYS;
    }
    status = handle_lookup(&process->handles, (handle_t)handle, rights, &object);
    if (status != K_OK) return status;
    stream = (audio_stream_t *)object;
    if (!audio_stream_is_valid(stream)) {
        status = K_EINVAL;
        goto done;
    }
    arguments.bytes_returned = 0U;
    switch (arguments.code) {
    case OS_AUDIO_CONTROL_CONFIGURE: {
        os_audio_stream_config_t config;
        audio_format_t format;
        if (arguments.buffer == 0U || arguments.buffer_size < sizeof(config)) {
            status = K_EINVAL;
            break;
        }
        status = copy_from_user(&config,
            (const void __user *)(uintptr_t)arguments.buffer, sizeof(config));
        if (status == K_OK) status = audio_uapi_to_kernel(&config, &format);
        if (status == K_OK) {
            status = audio_stream_configure(stream, &format);
            if (status == K_OK) status = audio_backend_configure(stream);
            if (status == K_OK) arguments.bytes_returned = sizeof(config);
        }
        break;
    }
    case OS_AUDIO_CONTROL_START:
        if (arguments.buffer != 0U || arguments.buffer_size != 0U) {
            status = K_EINVAL;
            break;
        }
        status = audio_backend_start(stream);
        if (status == K_ENOENT) status = K_OK;
        if (status == K_OK) {
            status = audio_stream_start(stream);
            if (status != K_OK) (void)audio_backend_stop(stream);
        }
        break;
    case OS_AUDIO_CONTROL_STOP:
        if (arguments.buffer != 0U || arguments.buffer_size != 0U) {
            status = K_EINVAL;
            break;
        }
        status = audio_backend_stop(stream);
        if (status == K_ENOENT) status = K_OK;
        if (status == K_OK) status = audio_stream_stop(stream);
        break;
    case OS_AUDIO_CONTROL_QUEUE:
        expected = audio_stream_bytes_for_frames(stream, arguments.frames);
        if (expected == 0U || arguments.period == UINT32_MAX ||
            audio_stream_direction(stream) == AUDIO_CAPTURE) {
            status = K_EINVAL;
            break;
        }
        if (arguments.buffer == 0U || arguments.buffer_size != expected) {
            status = K_EINVAL;
            break;
        }
        temporary = kzalloc((size_t)expected, 0);
        if (temporary == 0) {
            status = K_ENOMEM;
            break;
        }
        status = copy_from_user(temporary,
            (const void __user *)(uintptr_t)arguments.buffer, (size_t)expected);
        if (status == K_OK) status = audio_stream_period_write(
            stream, arguments.period, temporary, expected);
        if (status == K_OK) status = audio_stream_queue(stream, arguments.period,
                                                         arguments.frames);
        if (status == K_OK) {
            kstatus_t backend = audio_backend_queue(stream, arguments.period,
                                                     arguments.frames);
            if (backend != K_OK && backend != K_ENOENT) status = backend;
        }
        kfree(temporary);
        temporary = 0;
        if (status == K_OK) arguments.bytes_returned = expected;
        break;
    case OS_AUDIO_CONTROL_COMPLETE:
        expected = audio_stream_bytes_for_frames(stream, arguments.frames);
        if (expected == 0U || arguments.period == UINT32_MAX) {
            status = K_EINVAL;
            break;
        }
        if (audio_stream_direction(stream) == AUDIO_CAPTURE) {
            if (arguments.buffer == 0U || arguments.buffer_size != expected) {
                status = K_EINVAL;
                break;
            }
            temporary = kzalloc((size_t)expected, 0);
            if (temporary == 0) {
                status = K_ENOMEM;
                break;
            }
        } else if (arguments.buffer != 0U || arguments.buffer_size != 0U) {
            status = K_EINVAL;
            break;
        }
        status = audio_stream_complete(stream, arguments.period, arguments.frames);
        if (status == K_OK && temporary != 0) {
            status = audio_stream_period_read(stream, arguments.period,
                                              temporary, expected);
            if (status == K_OK) status = copy_to_user(
                (void __user *)(uintptr_t)arguments.buffer, temporary,
                (size_t)expected);
        }
        if (temporary != 0) kfree(temporary);
        temporary = 0;
        if (status == K_OK) arguments.bytes_returned = expected;
        break;
    case OS_AUDIO_CONTROL_RECOVER:
        status = arguments.buffer == 0U && arguments.buffer_size == 0U ?
                 audio_stream_recover(stream) : K_EINVAL;
        break;
    case OS_AUDIO_CONTROL_RESET:
        if (arguments.buffer != 0U || arguments.buffer_size != 0U) {
            status = K_EINVAL;
            break;
        }
        status = audio_backend_reset(stream);
        if (status == K_ENOENT) status = K_OK;
        if (status == K_OK) status = audio_stream_controller_reset(stream);
        break;
    case OS_AUDIO_CONTROL_DISCONNECT:
        if (arguments.buffer != 0U || arguments.buffer_size != 0U) {
            status = K_EINVAL;
            break;
        }
        status = audio_backend_disconnect(stream);
        if (status == K_ENOENT) status = K_OK;
        if (status == K_OK) status = audio_stream_disconnect(stream);
        break;
    case OS_AUDIO_CONTROL_GET_STATS: {
        audio_stream_stats_t kernel_stats = {0};
        os_audio_stream_stats_t user_stats = {0};
        if (arguments.buffer == 0U ||
            arguments.buffer_size < sizeof(user_stats)) {
            status = K_EINVAL;
            break;
        }
        audio_stream_get_stats(stream, &kernel_stats);
        user_stats.hdr.size = sizeof(user_stats);
        user_stats.hdr.version = OS_SYSCALL_ABI_VERSION;
        user_stats.queued_frames = kernel_stats.queued_frames;
        user_stats.mixed_frames = kernel_stats.completed_frames;
        user_stats.underruns = kernel_stats.underruns;
        user_stats.overruns = kernel_stats.overruns;
        user_stats.device_generation = kernel_stats.device_generation;
        user_stats.state = audio_uapi_state(kernel_stats.state);
        status = copy_to_user((void __user *)(uintptr_t)arguments.buffer,
                              &user_stats, sizeof(user_stats));
        if (status == K_OK) arguments.bytes_returned = sizeof(user_stats);
        break;
    }
    default:
        status = K_ENOSYS;
        break;
    }
done:
    if (temporary != 0) kfree(temporary);
    object_put(object);
    if (status != K_OK) return status;
    return copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                        &arguments, sizeof(arguments));
}

/* DEVICE_ENUMERATE(args)：按紧凑索引返回公开设备信息，不暴露内核地址。 */
static int64_t sys_device_enumerate(uint64_t arguments_pointer, uint64_t unused1,
                                    uint64_t unused2, uint64_t unused3,
                                    uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    os_device_enumerate_t arguments;
    os_device_info_t info = {0};
    device_t *device = 0;
    kstatus_t status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.reserved != 0U || arguments.output == 0U ||
        arguments.output_size < sizeof(info)) return K_EINVAL;
    status = device_get_by_index(arguments.index, &device);
    if (status != K_OK) return status;
    info.hdr.size = sizeof(info);
    info.hdr.version = OS_SYSCALL_ABI_VERSION;
    info.device_id = device->device_id;
    info.class_id = device->class_id;
    info.state = atomic_load_explicit(&device->state, memory_order_acquire);
    status = copy_to_user((void __user *)(uintptr_t)arguments.output,
                          &info, sizeof(info));
    object_put(device);
    if (status != K_OK) return status;
    arguments.bytes_returned = sizeof(info);
    return copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                        &arguments, sizeof(arguments));
}

/* DEVICE_OPEN(args)：按稳定 device_id 查找设备并生成带权限的 capability。 */
static int64_t sys_device_open(uint64_t arguments_pointer, uint64_t unused1,
                               uint64_t unused2, uint64_t unused3,
                               uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0) return K_EPERM;

    os_device_open_t arguments;
    kstatus_t status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.desired_rights == 0U ||
        (arguments.desired_rights & ~OS_DEVICE_RIGHT_ALL) != 0U ||
        arguments.reserved != 0U) return K_EINVAL;

    device_t *device = 0;
    status = device_get_by_id(arguments.device_id, &device);
    if (status != K_OK) return status;
    handle_t handle = 0;
    status = handle_create(&process->handles, device, arguments.desired_rights,
                           &handle);
    if (status == K_OK) {
        arguments.handle = handle;
        status = copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                              &arguments, sizeof(arguments));
    }
    if (status != K_OK && handle != OS_INVALID_HANDLE) {
        (void)handle_close(&process->handles, handle);
    }
    object_put(device);
    return status;
}

/* DEVICE_CONTROL(handle, args)：当前先提供查询、复位和电源状态控制。 */
static int64_t sys_device_control(uint64_t handle, uint64_t arguments_pointer,
                                  uint64_t unused2, uint64_t unused3,
                                  uint64_t unused4, uint64_t unused5) {
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0) return K_EPERM;

    os_device_control_t arguments;
    kstatus_t status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.flags != 0U || arguments.reserved != 0U) return K_EINVAL;
    arguments.bytes_returned = 0;

    uint32_t rights = OS_DEVICE_RIGHT_CONTROL;
    switch (arguments.code) {
    case OS_DEVICE_CONTROL_QUERY:
        rights |= OS_DEVICE_RIGHT_QUERY;
        if (arguments.output == 0U || arguments.output_size < sizeof(os_device_info_t)) {
            return K_EINVAL;
        }
        break;
    case OS_DEVICE_CONTROL_RESET:
        rights |= OS_DEVICE_RIGHT_RESET;
        break;
    case OS_DEVICE_CONTROL_SET_POWER:
        rights |= OS_DEVICE_RIGHT_POWER;
        if (arguments.level_or_state > OS_DEVICE_POWER_SUSPENDED) return K_EINVAL;
        break;
    default:
        return K_ENOSYS;
    }

    void *object = 0;
    status = handle_lookup(&process->handles, (handle_t)handle, rights, &object);
    if (status != K_OK) return status;
    device_t *device = (device_t *)object;
    if (device->object.type != KOBJECT_TYPE_DEVICE) {
        status = K_EINVAL;
    } else if (atomic_load_explicit(&device->state, memory_order_acquire) >=
               DEVICE_REMOVING) {
        status = K_EDEVREMOVED;
    } else if (arguments.code == OS_DEVICE_CONTROL_QUERY) {
        os_device_info_t info = {0};
        info.hdr.size = sizeof(info);
        info.hdr.version = OS_SYSCALL_ABI_VERSION;
        info.device_id = device->device_id;
        info.class_id = device->class_id;
        info.state = atomic_load_explicit(&device->state, memory_order_acquire);
        status = copy_to_user((void __user *)(uintptr_t)arguments.output,
                              &info, sizeof(info));
        if (status == K_OK) arguments.bytes_returned = sizeof(info);
    } else if (arguments.code == OS_DEVICE_CONTROL_RESET) {
        status = device_reset(device, arguments.level_or_state);
    } else if (arguments.level_or_state == OS_DEVICE_POWER_ACTIVE) {
        status = device_resume(device);
    } else {
        status = device_suspend(device);
    }
    object_put(object);
    if (status != K_OK) return status;
    status = copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                          &arguments, sizeof(arguments));
    return status;
}

/* PORT_CREATE(kind, capacity, output_handle)：创建消息端口或完成端口。 */
static int64_t sys_port_create(uint64_t kind, uint64_t capacity,
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
static int64_t sys_completion_wait(uint64_t handle, uint64_t timeout_ns,
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
static int64_t sys_clock_get(uint64_t clock_id, uint64_t output_pointer,
                             uint64_t unused2, uint64_t unused3,
                             uint64_t unused4, uint64_t unused5) {
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    if (clock_id != 0U) return K_EINVAL;
    uint64_t nanoseconds = x86_tsc_to_ns(x86_read_tsc());
    os_timespec_t value = {
        .seconds = (int64_t)(nanoseconds / 1000000000ULL),
        .nanoseconds = (int32_t)(nanoseconds % 1000000000ULL),
        .reserved = 0,
    };
    return copy_to_user((void __user *)(uintptr_t)output_pointer,
                        &value, sizeof(value));
}

/* TIMER_CREATE(delay_ns, period_ns, output_handle)。定时器本身是可等待对象。 */
static int64_t sys_timer_create(uint64_t delay_ns, uint64_t period_ns,
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
static int64_t sys_port_send(uint64_t handle, uint64_t buffer, uint64_t size,
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
static int64_t sys_port_receive(uint64_t handle, uint64_t buffer, uint64_t capacity,
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

static int64_t sys_wait_one(uint64_t handle, uint64_t timeout_ns,
                            uint64_t output_pointer, uint64_t unused3,
                            uint64_t unused4, uint64_t unused5) {
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0) return K_EPERM;
    void *object = 0;
    kstatus_t status = handle_lookup(&process->handles, (handle_t)handle,
                                     OBJECT_RIGHT_WAIT, &object);
    if (status != K_OK) return status;
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
    object_put(object);
    return status;
}

static int64_t sys_wait_many(uint64_t arguments_pointer, uint64_t unused1,
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

static int64_t sys_futex_wait(uint64_t address, uint64_t expected, uint64_t timeout_ns,
                              uint64_t flags, uint64_t unused4, uint64_t unused5) {
    (void)unused4;
    (void)unused5;
    if (flags != 0 || expected > UINT32_MAX) return K_EINVAL;
    process_t *process = current_process();
    if (process == 0) return K_EPERM;
    return futex_wait(process, (uint32_t __user *)(uintptr_t)address,
                      (uint32_t)expected, timeout_ns);
}

static int64_t sys_futex_wake(uint64_t address, uint64_t maximum, uint64_t flags,
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

static int64_t sys_file_open(uint64_t path, uint64_t flags, uint64_t mode,
                             uint64_t output_pointer, uint64_t unused4,
                             uint64_t unused5) {
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0) return K_EPERM;
    file_t *file = 0;
    kstatus_t status = vfs_open((const char __user *)(uintptr_t)path,
                                (uint32_t)flags, (uint32_t)mode, &file);
    if (status != K_OK) return status;
    handle_t handle = 0;
    status = handle_create(&process->handles, file, file->rights, &handle);
    if (status == K_OK) {
        status = copy_to_user((void __user *)(uintptr_t)output_pointer,
                              &handle, sizeof(handle));
    }
    if (status != K_OK && handle != 0) (void)handle_close(&process->handles, handle);
    object_put(file);
    return status;
}

static int64_t sys_file_enumerate(uint64_t arguments_pointer, uint64_t unused1,
                                  uint64_t unused2, uint64_t unused3,
                                  uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    os_file_enumerate_t arguments;
    kstatus_t status;
    if (process == 0) return K_EPERM;
    status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.path == 0U) return K_EINVAL;
    status = vfs_enumerate((const char __user *)(uintptr_t)arguments.path,
                           arguments.index, &arguments.info);
    if (status != K_OK) return status;
    return copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                        &arguments, sizeof(arguments));
}

static int64_t sys_file_seek(uint64_t arguments_pointer, uint64_t unused1,
                             uint64_t unused2, uint64_t unused3,
                             uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    os_file_seek_t arguments;
    void *object = 0;
    file_t *file;
    kstatus_t status;
    process_t *process = current_process();
    if (process == 0) return K_EPERM;
    status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.reserved != 0U || arguments.whence > OS_FILE_SEEK_END) {
        return K_EINVAL;
    }
    status = handle_lookup(&process->handles, (handle_t)arguments.handle, 0, &object);
    if (status != K_OK) return status;
    file = (file_t *)object;
    status = file->object.type == KOBJECT_TYPE_FILE ?
        vfs_seek(file, arguments.offset, arguments.whence, &arguments.position) : K_EBADF;
    object_put(object);
    if (status != K_OK) return status;
    return copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                        &arguments, sizeof(arguments));
}

static int64_t sys_file_stat(uint64_t arguments_pointer, uint64_t unused1,
                             uint64_t unused2, uint64_t unused3,
                             uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    os_file_stat_t arguments;
    kstatus_t status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.path == 0U) return K_EINVAL;
    status = vfs_stat((const char __user *)(uintptr_t)arguments.path,
                      &arguments.info);
    if (status != K_OK) return status;
    return copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                        &arguments, sizeof(arguments));
}

static int64_t sys_file_truncate(uint64_t arguments_pointer, uint64_t unused1,
                                 uint64_t unused2, uint64_t unused3,
                                 uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    os_file_truncate_t arguments;
    void *object = 0;
    process_t *process = current_process();
    kstatus_t status;
    if (process == 0) return K_EPERM;
    status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments))) return K_EINVAL;
    status = handle_lookup(&process->handles, (handle_t)arguments.handle,
                           FILE_RIGHT_WRITE, &object);
    if (status != K_OK) return status;
    status = ((file_t *)object)->object.type == KOBJECT_TYPE_FILE ?
        vfs_truncate_kernel((file_t *)object, arguments.size) : K_EBADF;
    object_put(object);
    return status;
}

static int64_t sys_file_remove(uint64_t arguments_pointer, uint64_t unused1,
                               uint64_t unused2, uint64_t unused3,
                               uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    os_file_path_op_t arguments;
    kstatus_t status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.path == 0U || arguments.mode != 0U || arguments.reserved != 0U) {
        return K_EINVAL;
    }
    return vfs_remove((const char __user *)(uintptr_t)arguments.path);
}

static int64_t sys_file_mkdir(uint64_t arguments_pointer, uint64_t unused1,
                              uint64_t unused2, uint64_t unused3,
                              uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    os_file_path_op_t arguments;
    kstatus_t status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.path == 0U || arguments.reserved != 0U) return K_EINVAL;
    return vfs_mkdir((const char __user *)(uintptr_t)arguments.path, arguments.mode);
}

static int64_t sys_file_read(uint64_t handle, uint64_t buffer, uint64_t length,
                             uint64_t output_bytes, uint64_t unused4,
                             uint64_t unused5) {
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0 || length > (uint64_t)SIZE_MAX) return K_EINVAL;
    void *object = 0;
    kstatus_t status = handle_lookup(&process->handles, (handle_t)handle,
                                     FILE_RIGHT_READ, &object);
    if (status != K_OK) return status;
    file_t *file = (file_t *)object;
    if (file->object.type != KOBJECT_TYPE_FILE) {
        object_put(file);
        return K_EINVAL;
    }
    uint64_t bytes = 0;
    status = vfs_read(file, (void __user *)(uintptr_t)buffer, (size_t)length, &bytes);
    if (status == K_OK) {
        status = copy_to_user((void __user *)(uintptr_t)output_bytes,
                              &bytes, sizeof(bytes));
    }
    object_put(file);
    return status;
}

static int64_t sys_file_write(uint64_t handle, uint64_t buffer, uint64_t length,
                              uint64_t output_bytes, uint64_t unused4,
                              uint64_t unused5) {
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0 || length > (uint64_t)SIZE_MAX) return K_EINVAL;
    void *object = 0;
    kstatus_t status = handle_lookup(&process->handles, (handle_t)handle,
                                     FILE_RIGHT_WRITE, &object);
    if (status != K_OK) return status;
    file_t *file = (file_t *)object;
    if (file->object.type != KOBJECT_TYPE_FILE) {
        object_put(file);
        return K_EINVAL;
    }
    uint64_t bytes = 0;
    status = vfs_write(file, (const void __user *)(uintptr_t)buffer,
                       (size_t)length, &bytes);
    if (status == K_OK) {
        status = copy_to_user((void __user *)(uintptr_t)output_bytes,
                              &bytes, sizeof(bytes));
    }
    object_put(file);
    return status;
}

static int64_t sys_file_fsync(uint64_t handle, uint64_t unused1, uint64_t unused2,
                              uint64_t unused3, uint64_t unused4,
                              uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0) return K_EPERM;
    void *object = 0;
    kstatus_t status = handle_lookup(&process->handles, (handle_t)handle, 0, &object);
    if (status != K_OK) return status;
    file_t *file = (file_t *)object;
    status = file->object.type == KOBJECT_TYPE_FILE ? vfs_fsync(file) : K_EINVAL;
    object_put(file);
    return status;
}

/* IO_SUBMIT(args)：将用户向量复制到内核缓冲区后提交到设备。 */
static int64_t sys_io_submit(uint64_t arguments_pointer, uint64_t unused1,
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
    context = 0; /* request 已接管上下文所有权。 */
    status = io_request_set_completion_port(
        request, (completion_port_t *)port_object, arguments.user_key);
    if (status != K_OK) goto cleanup;
    status = io_request_register_user(request);
    if (status != K_OK) goto cleanup;
    object_get(request); /* 覆盖同步设备可能在 io_submit 内立即完成的情况。 */
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
            atomic_load_explicit(&request->state, memory_order_acquire) !=
                IOREQ_COMPLETED) {
            /* 尚未提交的错误路径仍需摘除登记表。 */
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

/* IO_CANCEL(request_id)：只允许取消当前进程提交的请求。 */
static int64_t sys_io_cancel(uint64_t request_id, uint64_t unused1,
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

static int64_t sys_socket_create(uint64_t family, uint64_t type, uint64_t protocol,
                                 uint64_t output_pointer, uint64_t unused4,
                                 uint64_t unused5) {
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0 || family > UINT16_MAX || type > UINT16_MAX ||
        protocol > UINT16_MAX) return K_EINVAL;
    socket_t *socket = 0;
    kstatus_t status = socket_create((uint16_t)family, (uint16_t)type,
                                     (uint16_t)protocol, &socket);
    if (status != K_OK) return status;
    handle_t handle = 0;
    status = handle_create(&process->handles, socket, SOCKET_RIGHT_ALL, &handle);
    if (status == K_OK) {
        status = copy_to_user((void __user *)(uintptr_t)output_pointer,
                              &handle, sizeof(handle));
    }
    if (status != K_OK && handle != 0) (void)handle_close(&process->handles, handle);
    object_put(socket);
    return status;
}

/* NET_GET_STATUS(args)：向用户态网络管理器提供稳定的链路和地址快照。 */
static int64_t sys_net_get_status(uint64_t arguments_pointer, uint64_t unused1,
                                  uint64_t unused2, uint64_t unused3,
                                  uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    os_net_status_t arguments;
    net_manager_status_t status = {0};
    kstatus_t result;
    if (arguments_pointer == 0U) return K_EINVAL;
    result = copy_from_user(&arguments,
                            (const void __user *)(uintptr_t)arguments_pointer,
                            sizeof(arguments));
    if (result != K_OK) return result;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments))) return K_EINVAL;
    if (!net_manager_get_status(&status)) return K_EIO;
    arguments.flags = 0U;
    if (status.hardware_present) arguments.flags |= OS_NET_STATUS_HARDWARE_PRESENT;
    if (status.link_up) arguments.flags |= OS_NET_STATUS_LINK_UP;
    if (status.ipv6_configured) arguments.flags |= OS_NET_STATUS_IPV6_CONFIGURED;
    arguments.ipv4_address = status.ipv4_address;
    arguments.ipv4_gateway = status.ipv4_gateway;
    arguments.ipv4_prefix_length = status.ipv4_prefix_length;
    arguments.reserved[0] = 0U;
    arguments.reserved[1] = 0U;
    arguments.reserved[2] = 0U;
    for (uint32_t index = 0U; index < sizeof(arguments.ipv6_address); ++index) {
        arguments.ipv6_address[index] = status.ipv6_address[index];
    }
    arguments.link_transitions = status.link_transitions;
    arguments.reset_count = status.reset_count;
    for (uint32_t index = 0U; index < sizeof(arguments.mac); ++index) {
        arguments.mac[index] = status.mac[index];
    }
    arguments.mac_reserved[0] = 0U;
    arguments.mac_reserved[1] = 0U;
    return copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                        &arguments, sizeof(arguments));
}

/*
 * NET_SUBSCRIBE(message_port): bind the system network daemon's waitable
 * message port to link-state changes. OS_INVALID_HANDLE unsubscribes.
 */
static int64_t sys_net_subscribe(uint64_t port_handle, uint64_t unused1,
                                 uint64_t unused2, uint64_t unused3,
                                 uint64_t unused4, uint64_t unused5) {
    process_t *process = current_process();
    void *object = 0;
    kstatus_t status;

    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;

    if (process == 0) return K_EPERM;
    if (port_handle == OS_INVALID_HANDLE) return net_manager_subscribe(0);

    status = handle_lookup(&process->handles,
                           (handle_t)port_handle,
                           MESSAGE_PORT_RIGHT_WRITE,
                           &object);
    if (status != K_OK) return status;

    if (((message_port_t *)object)->object.type != KOBJECT_TYPE_MESSAGE_PORT) {
        status = K_EINVAL;
    } else {
        status = net_manager_subscribe((message_port_t *)object);
    }

    object_put(object);
    return status;
}

/* NET_SET_IPV4(args)：只有系统管理 capability 能修改地址和默认路由。 */
static int64_t sys_net_set_ipv4(uint64_t arguments_pointer, uint64_t unused1,
                                uint64_t unused2, uint64_t unused3,
                                uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    os_net_set_ipv4_config_t arguments;
    if (process == 0 || process->token == 0 || arguments_pointer == 0U) {
        return K_EPERM;
    }
    if ((process->token->capabilities & SECURITY_CAPABILITY_SYSTEM_ADMIN) == 0U) {
        return K_EACCES;
    }
    kstatus_t status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.reserved[0] != 0U || arguments.reserved[1] != 0U ||
        arguments.reserved[2] != 0U || arguments.prefix_length > 32U ||
        (arguments.gateway != 0U && arguments.address == 0U)) return K_EINVAL;
    return e1000_set_ipv4_config(arguments.address, arguments.prefix_length,
                                  arguments.gateway);
}

static int64_t sys_socket_bind(uint64_t handle, uint64_t address, uint64_t port,
                               uint64_t unused3, uint64_t unused4, uint64_t unused5) {
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0 || address > UINT32_MAX || port > UINT16_MAX) return K_EINVAL;
    void *object = 0;
    kstatus_t status = handle_lookup(&process->handles, (handle_t)handle,
                                     SOCKET_RIGHT_WRITE, &object);
    if (status != K_OK) return status;
    socket_t *socket = (socket_t *)object;
    status = socket->object.type == KOBJECT_TYPE_SOCKET ?
             socket_bind(socket, (uint32_t)address, (uint16_t)port) : K_EINVAL;
    object_put(object);
    return status;
}

static int64_t sys_socket_connect(uint64_t handle, uint64_t address, uint64_t port,
                                  uint64_t unused3, uint64_t unused4, uint64_t unused5) {
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0 || address > UINT32_MAX || port > UINT16_MAX) return K_EINVAL;
    void *object = 0;
    kstatus_t status = handle_lookup(&process->handles, (handle_t)handle,
                                     SOCKET_RIGHT_WRITE, &object);
    if (status != K_OK) return status;
    socket_t *socket = (socket_t *)object;
    status = socket->object.type == KOBJECT_TYPE_SOCKET ?
             socket_connect(socket, (uint32_t)address, (uint16_t)port) : K_EINVAL;
    object_put(object);
    return status;
}

/* SOCKET_BIND6(handle, address[16], port)：为 IPv6 UDP socket 绑定端点。 */
static int64_t sys_socket_bind6(uint64_t handle, uint64_t address_pointer, uint64_t port,
                                uint64_t unused3, uint64_t unused4, uint64_t unused5) {
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    uint8_t address[16];
    void *object = 0;
    if (process == 0 || address_pointer == 0U || port == 0U || port > UINT16_MAX ||
        copy_from_user(address, (const void __user *)(uintptr_t)address_pointer,
                       sizeof(address)) != K_OK) return K_EACCES;
    kstatus_t status = handle_lookup(&process->handles, (handle_t)handle,
                                     SOCKET_RIGHT_WRITE, &object);
    if (status != K_OK) return status;
    socket_t *socket = (socket_t *)object;
    status = socket->object.type == KOBJECT_TYPE_SOCKET ?
             socket_bind_ipv6(socket, address, (uint16_t)port) : K_EINVAL;
    object_put(object);
    return status;
}

/* SOCKET_CONNECT6(handle, address[16], port)：连接 IPv6 UDP 对端。 */
static int64_t sys_socket_connect6(uint64_t handle, uint64_t address_pointer, uint64_t port,
                                   uint64_t unused3, uint64_t unused4, uint64_t unused5) {
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    uint8_t address[16];
    void *object = 0;
    if (process == 0 || address_pointer == 0U || port == 0U || port > UINT16_MAX ||
        copy_from_user(address, (const void __user *)(uintptr_t)address_pointer,
                       sizeof(address)) != K_OK) return K_EACCES;
    kstatus_t status = handle_lookup(&process->handles, (handle_t)handle,
                                     SOCKET_RIGHT_WRITE, &object);
    if (status != K_OK) return status;
    socket_t *socket = (socket_t *)object;
    status = socket->object.type == KOBJECT_TYPE_SOCKET ?
             socket_connect_ipv6(socket, address, (uint16_t)port) : K_EINVAL;
    object_put(object);
    return status;
}

static int64_t sys_socket_listen(uint64_t handle, uint64_t backlog,
                                 uint64_t unused2, uint64_t unused3,
                                 uint64_t unused4, uint64_t unused5) {
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0 || backlog > UINT32_MAX) return K_EINVAL;
    void *object = 0;
    kstatus_t status = handle_lookup(&process->handles, (handle_t)handle,
                                     SOCKET_RIGHT_WRITE, &object);
    if (status != K_OK) return status;
    socket_t *socket = (socket_t *)object;
    status = socket->object.type == KOBJECT_TYPE_SOCKET ?
             socket_listen(socket, (uint32_t)backlog) : K_EINVAL;
    object_put(object);
    return status;
}

static int64_t sys_socket_accept(uint64_t handle, uint64_t timeout_ns,
                                 uint64_t output_handle, uint64_t unused3,
                                 uint64_t unused4, uint64_t unused5) {
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0) return K_EPERM;
    void *object = 0;
    kstatus_t status = handle_lookup(&process->handles, (handle_t)handle,
                                     SOCKET_RIGHT_WAIT, &object);
    if (status != K_OK) return status;
    socket_t *listener = (socket_t *)object;
    socket_t *accepted = 0;
    if (listener->object.type != KOBJECT_TYPE_SOCKET) {
        status = K_EINVAL;
    } else {
        status = socket_accept(listener, timeout_ns, &accepted);
    }
    handle_t accepted_handle = 0;
    if (status == K_OK) {
        status = handle_create(&process->handles, accepted, SOCKET_RIGHT_ALL,
                               &accepted_handle);
        if (status == K_OK) {
            status = copy_to_user((void __user *)(uintptr_t)output_handle,
                                  &accepted_handle, sizeof(accepted_handle));
        }
        if (status != K_OK && accepted_handle != 0) {
            (void)handle_close(&process->handles, accepted_handle);
        }
        object_put(accepted); /* 丢弃 accept 队列转移给调用方的引用。 */
    }
    object_put(object);
    return status;
}

static int64_t sys_socket_send(uint64_t handle, uint64_t buffer, uint64_t length,
                               uint64_t address, uint64_t port, uint64_t flags) {
    process_t *process = current_process();
    if (process == 0 || length > SOCKET_MAX_PAYLOAD || address > UINT32_MAX ||
        port > UINT16_MAX || flags != 0U) return K_EINVAL;
    uint8_t payload[SOCKET_MAX_PAYLOAD];
    if (length != 0 && copy_from_user(payload,
                                      (const void __user *)(uintptr_t)buffer,
                                      (size_t)length) != K_OK) return K_EACCES;
    void *object = 0;
    kstatus_t status = handle_lookup(&process->handles, (handle_t)handle,
                                     SOCKET_RIGHT_WRITE, &object);
    if (status != K_OK) return status;
    socket_t *socket = (socket_t *)object;
    uint64_t bytes = 0;
    status = socket->object.type == KOBJECT_TYPE_SOCKET ?
             socket_send(socket, payload, (size_t)length, (uint32_t)address,
                         (uint16_t)port, &bytes) : K_EINVAL;
    object_put(object);
    return status == K_OK ? (int64_t)bytes : status;
}

/* SOCKET_SEND6(handle, buffer, length, address[16], port, flags)。 */
static int64_t sys_socket_send6(uint64_t handle, uint64_t buffer, uint64_t length,
                                uint64_t address_pointer, uint64_t port, uint64_t flags) {
    process_t *process = current_process();
    uint8_t payload[SOCKET_MAX_PAYLOAD];
    uint8_t address[16];
    uint64_t bytes = 0U;
    void *object = 0;
    if (process == 0 || length > SOCKET_MAX_PAYLOAD || address_pointer == 0U ||
        port > UINT16_MAX || flags != 0U ||
        copy_from_user(address, (const void __user *)(uintptr_t)address_pointer,
                       sizeof(address)) != K_OK) return K_EACCES;
    if (length != 0U && copy_from_user(payload,
                                       (const void __user *)(uintptr_t)buffer,
                                       (size_t)length) != K_OK) return K_EACCES;
    kstatus_t status = handle_lookup(&process->handles, (handle_t)handle,
                                     SOCKET_RIGHT_WRITE, &object);
    if (status != K_OK) return status;
    socket_t *socket = (socket_t *)object;
    status = socket->object.type == KOBJECT_TYPE_SOCKET ?
             socket_send_ipv6(socket, payload, (size_t)length, address,
                              (uint16_t)port, &bytes) : K_EINVAL;
    object_put(object);
    return status == K_OK ? (int64_t)bytes : status;
}

/* SOCKET_SEND_ASYNC(args)：提交时复制负载，完成后通过 completion port 通知。 */
static int64_t sys_socket_send_async(uint64_t arguments_pointer,
                                     uint64_t unused1, uint64_t unused2,
                                     uint64_t unused3, uint64_t unused4,
                                     uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    os_socket_async_send_t arguments;
    uint8_t payload[SOCKET_MAX_PAYLOAD];
    void *socket_object = 0;
    void *port_object = 0;
    uint64_t request_id = 0U;
    kstatus_t status;
    if (process == 0) return K_EPERM;
    status = copy_from_user(&arguments,
                            (const void __user *)(uintptr_t)arguments_pointer,
                            sizeof(arguments));
    if (status != K_OK || !versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.flags != 0U || arguments.length > SOCKET_MAX_PAYLOAD) return K_EINVAL;
    if (arguments.length != 0U &&
        copy_from_user(payload, (const void __user *)(uintptr_t)arguments.buffer,
                       (size_t)arguments.length) != K_OK) return K_EACCES;
    status = handle_lookup(&process->handles, (handle_t)arguments.socket,
                           SOCKET_RIGHT_WRITE, &socket_object);
    if (status != K_OK) return status;
    status = handle_lookup(&process->handles, (handle_t)arguments.completion_port,
                           COMPLETION_PORT_RIGHT_WRITE, &port_object);
    if (status == K_OK &&
        ((socket_t *)socket_object)->object.type == KOBJECT_TYPE_SOCKET &&
        ((completion_port_t *)port_object)->object.type ==
            KOBJECT_TYPE_COMPLETION_PORT) {
        status = socket_send_async((socket_t *)socket_object, payload,
                                   (size_t)arguments.length,
                                   (uint32_t)arguments.address,
                                   (uint16_t)arguments.port,
                                   (completion_port_t *)port_object,
                                   arguments.user_key, &request_id);
    } else if (status == K_OK) {
        status = K_EINVAL;
    }
    if (port_object != 0) object_put(port_object);
    object_put(socket_object);
    if (status == K_OK) return (int64_t)request_id;
    return status;
}

/* SOCKET_SEND_ASYNC6(args)：复制 IPv6 地址和负载后投递 deferred 发送任务。 */
static int64_t sys_socket_send_async6(uint64_t arguments_pointer,
                                      uint64_t unused1, uint64_t unused2,
                                      uint64_t unused3, uint64_t unused4,
                                      uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    os_socket_ipv6_async_send_t arguments;
    uint8_t payload[SOCKET_MAX_PAYLOAD];
    void *socket_object = 0;
    void *port_object = 0;
    uint64_t request_id = 0U;
    kstatus_t status;
    if (process == 0 || arguments_pointer == 0U) return K_EPERM;
    status = copy_from_user(&arguments,
                            (const void __user *)(uintptr_t)arguments_pointer,
                            sizeof(arguments));
    if (status != K_OK || !versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.flags != 0U || arguments.port == 0U ||
        arguments.length > SOCKET_MAX_PAYLOAD) return K_EINVAL;
    if (arguments.length != 0U &&
        copy_from_user(payload, (const void __user *)(uintptr_t)arguments.buffer,
                       (size_t)arguments.length) != K_OK) return K_EACCES;
    status = handle_lookup(&process->handles, (handle_t)arguments.socket,
                           SOCKET_RIGHT_WRITE, &socket_object);
    if (status != K_OK) return status;
    status = handle_lookup(&process->handles, (handle_t)arguments.completion_port,
                           COMPLETION_PORT_RIGHT_WRITE, &port_object);
    if (status == K_OK &&
        ((socket_t *)socket_object)->object.type == KOBJECT_TYPE_SOCKET &&
        ((completion_port_t *)port_object)->object.type == KOBJECT_TYPE_COMPLETION_PORT) {
        status = socket_send_async_ipv6((socket_t *)socket_object, payload,
                                        (size_t)arguments.length, arguments.address,
                                        arguments.port, (completion_port_t *)port_object,
                                        arguments.user_key, &request_id);
    } else if (status == K_OK) {
        status = K_EINVAL;
    }
    if (port_object != 0) object_put(port_object);
    object_put(socket_object);
    return status == K_OK ? (int64_t)request_id : status;
}

static int64_t sys_socket_recv(uint64_t handle, uint64_t buffer, uint64_t length,
                               uint64_t source_address, uint64_t source_port,
                               uint64_t timeout_ns) {
    process_t *process = current_process();
    if (process == 0 || length > SOCKET_MAX_PAYLOAD) return K_EINVAL;
    uint8_t payload[SOCKET_MAX_PAYLOAD];
    socket_ipv4_endpoint_t source = {0};
    uint64_t bytes = 0;
    void *object = 0;
    kstatus_t status = handle_lookup(&process->handles, (handle_t)handle,
                                     SOCKET_RIGHT_READ, &object);
    if (status != K_OK) return status;
    socket_t *socket = (socket_t *)object;
    status = socket->object.type == KOBJECT_TYPE_SOCKET ?
             socket_recv(socket, payload, (size_t)length, &source, timeout_ns,
                         &bytes) : K_EINVAL;
    if (status == K_OK) {
        if (bytes != 0 && copy_to_user((void __user *)(uintptr_t)buffer,
                                       payload, (size_t)bytes) != K_OK) status = K_EACCES;
        if (status == K_OK && copy_to_user(
                (void __user *)(uintptr_t)source_address, &source.address,
                sizeof(source.address)) != K_OK) status = K_EACCES;
        if (status == K_OK && copy_to_user(
                (void __user *)(uintptr_t)source_port, &source.port,
                sizeof(source.port)) != K_OK) status = K_EACCES;
    }
    object_put(object);
    return status == K_OK ? (int64_t)bytes : status;
}

/* SOCKET_RECV6(handle, buffer, length, source_address[16], source_port, timeout)。 */
static int64_t sys_socket_recv6(uint64_t handle, uint64_t buffer, uint64_t length,
                                uint64_t source_address, uint64_t source_port,
                                uint64_t timeout_ns) {
    process_t *process = current_process();
    uint8_t payload[SOCKET_MAX_PAYLOAD];
    socket_ipv6_endpoint_t source = {0};
    uint64_t bytes = 0U;
    void *object = 0;
    if (process == 0 || length > SOCKET_MAX_PAYLOAD || source_address == 0U ||
        source_port == 0U) return K_EINVAL;
    if (length != 0U && buffer == 0U) return K_EINVAL;
    kstatus_t status = handle_lookup(&process->handles, (handle_t)handle,
                                     SOCKET_RIGHT_READ, &object);
    if (status != K_OK) return status;
    socket_t *socket = (socket_t *)object;
    status = socket->object.type == KOBJECT_TYPE_SOCKET ?
             socket_recv_ipv6(socket, payload, (size_t)length, &source, timeout_ns,
                              &bytes) : K_EINVAL;
    if (status == K_OK) {
        if (bytes != 0U && copy_to_user((void __user *)(uintptr_t)buffer,
                                       payload, (size_t)bytes) != K_OK) status = K_EACCES;
        if (status == K_OK && copy_to_user(
                (void __user *)(uintptr_t)source_address, source.address,
                sizeof(source.address)) != K_OK) status = K_EACCES;
        if (status == K_OK && copy_to_user(
                (void __user *)(uintptr_t)source_port, &source.port,
                sizeof(source.port)) != K_OK) status = K_EACCES;
    }
    object_put(object);
    return status == K_OK ? (int64_t)bytes : status;
}

/* GPU_CREATE_CTX(args, output_handle)：当前后端允许软件 GPU 上下文。 */
static int64_t sys_gpu_create_context(uint64_t arguments_pointer,
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
static int64_t sys_gpu_alloc(uint64_t arguments_pointer, uint64_t output_pointer,
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
static int64_t sys_gpu_submit(uint64_t arguments_pointer, uint64_t unused1,
                              uint64_t unused2, uint64_t unused3,
                              uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0) return K_EPERM;
    os_gpu_submit_t arguments;
    kstatus_t status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.context == OS_INVALID_HANDLE ||
        arguments.command_buffer == OS_INVALID_HANDLE ||
        arguments.signal_value == 0U || arguments.command_length == 0U) return K_EINVAL;

    void *context_object = 0;
    status = handle_lookup(&process->handles, (handle_t)arguments.context,
                           GPU_CONTEXT_RIGHT_SUBMIT, &context_object);
    if (status != K_OK) return status;
    void *allocation_object = 0;
    status = handle_lookup(&process->handles, (handle_t)arguments.command_buffer,
                           GPU_ALLOCATION_RIGHT_READ, &allocation_object);
    if (status != K_OK) {
        object_put(context_object);
        return status;
    }
    gpu_fence_t *fence = 0;
    bool created_fence = false;
    handle_t fence_handle = (handle_t)arguments.signal_fence;
    if (fence_handle == OS_INVALID_HANDLE) {
        status = gpu_fence_create(&fence);
        if (status == K_OK) {
            status = handle_create(&process->handles, fence,
                                   GPU_FENCE_RIGHT_ALL, &fence_handle);
            if (status == K_OK) {
                created_fence = true;
                arguments.signal_fence = fence_handle;
            }
        }
    } else {
        void *fence_object = 0;
        status = handle_lookup(&process->handles, fence_handle,
                               GPU_FENCE_RIGHT_SIGNAL, &fence_object);
        if (status == K_OK) fence = (gpu_fence_t *)fence_object;
    }
    if (status == K_OK && (fence == 0 || fence->object.type != KOBJECT_TYPE_GPU_FENCE)) {
        status = K_EINVAL;
    }
    if (status == K_OK) {
        status = gpu_submit((gpu_context_t *)context_object,
                            (gpu_allocation_t *)allocation_object,
                            arguments.command_offset, arguments.command_length,
                            fence, arguments.signal_value);
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
    return status;
}

/* GPU_WAIT_FENCE(handle, value, timeout_ns)。 */
static int64_t sys_gpu_wait_fence(uint64_t handle, uint64_t value,
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
static int64_t sys_gpu_map(uint64_t arguments_pointer, uint64_t unused1,
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
static int64_t sys_display_get_info(uint64_t arguments_pointer,
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
static int64_t sys_input_read(uint64_t event_pointer, uint64_t timeout_ns,
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

static int64_t sys_window_register_manager(uint64_t unused0, uint64_t unused1,
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

static int64_t sys_window_create(uint64_t arguments_pointer, uint64_t unused1,
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
        (arguments.flags & ~(OS_WINDOW_VISIBLE | OS_WINDOW_RESIZABLE)) != 0U ||
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
        arguments.identifier = window->identifier;
        arguments.buffer_size = window->buffer_size;
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
static int64_t sys_window_update(uint64_t arguments_pointer, uint64_t unused1,
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

static int64_t sys_window_enumerate(uint64_t arguments_pointer, uint64_t unused1,
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

static int64_t sys_window_map(uint64_t arguments_pointer, uint64_t unused1,
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
    if (arguments.length != 0U && arguments.length != window->buffer_size) {
        window_server_put(window);
        return K_EINVAL;
    }
    status = window_server_map(window, process, arguments.address,
                               &arguments.address);
    arguments.length = window->buffer_size;
    window_server_put(window);
    if (status != K_OK) return status;
    return copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                        &arguments, sizeof(arguments));
}

static int64_t sys_window_set(uint64_t arguments_pointer, uint64_t unused1,
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

static int64_t sys_window_focus(uint64_t arguments_pointer, uint64_t unused1,
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

static int64_t sys_window_input_read(uint64_t event_pointer, uint64_t timeout_ns,
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

static int64_t sys_window_input_dispatch(uint64_t arguments_pointer,
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

static int64_t sys_window_event_read(uint64_t arguments_pointer, uint64_t unused1,
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

static int64_t sys_display_commit(uint64_t arguments_pointer,
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

/* TOKEN_QUERY(args)：返回当前进程的不可变身份与 capability 快照。 */
static int64_t sys_token_query(uint64_t arguments_pointer, uint64_t unused1,
                               uint64_t unused2, uint64_t unused3,
                               uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0 || process->token == 0) return K_EPERM;
    os_token_info_t info;
    kstatus_t status = copy_from_user(&info,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(info));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&info.hdr, sizeof(info)) || info.reserved != 0U) {
        return K_EINVAL;
    }
    const security_token_t *token = process->token;
    if (token->object.type != KOBJECT_TYPE_SECURITY_TOKEN) return K_EINVAL;
    info.uid = token->uid;
    info.gid = token->gid;
    info.groups = token->groups;
    info.privileges = token->privileges;
    info.capabilities = token->capabilities;
    info.flags = token->flags;
    status = copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                          &info, sizeof(info));
    return status;
}

static int64_t sys_debug_query(uint64_t query, uint64_t output, uint64_t output_size,
                               uint64_t unused3, uint64_t unused4, uint64_t unused5) {
    (void)output;
    (void)output_size;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    return query == 0U ? (int64_t)OS_SYSCALL_ABI_VERSION : K_EINVAL;
}

/* 稀疏编号仍用直接索引表，分派复杂度固定为 O(1)。 */
static const syscall_handler_t g_syscall_table[SYSCALL_TABLE_SIZE] = {
    [OS_SYS_THREAD_EXIT] = sys_thread_exit,
    [OS_SYS_PROCESS_EXIT] = sys_process_exit,
    [OS_SYS_THREAD_CREATE] = sys_thread_create,
    [OS_SYS_PROCESS_CREATE] = sys_process_create,
    [OS_SYS_PROCESS_EXEC] = sys_process_exec,
    [OS_SYS_VM_MAP] = sys_vm_map,
    [OS_SYS_VM_UNMAP] = sys_vm_unmap,
    [OS_SYS_VM_PROTECT] = sys_vm_protect,
    [OS_SYS_VM_SHARE] = sys_vm_share,
    [OS_SYS_HANDLE_CLOSE] = sys_handle_close,
    [OS_SYS_DEVICE_OPEN] = sys_device_open,
    [OS_SYS_DEVICE_CONTROL] = sys_device_control,
    [OS_SYS_DEVICE_ENUMERATE] = sys_device_enumerate,
    [OS_SYS_PORT_CREATE] = sys_port_create,
    [OS_SYS_PORT_SEND] = sys_port_send,
    [OS_SYS_PORT_RECEIVE] = sys_port_receive,
    [OS_SYS_COMPLETION_WAIT] = sys_completion_wait,
    [OS_SYS_CLOCK_GET] = sys_clock_get,
    [OS_SYS_TIMER_CREATE] = sys_timer_create,
    [OS_SYS_WAIT_ONE] = sys_wait_one,
    [OS_SYS_WAIT_MANY] = sys_wait_many,
    [OS_SYS_FUTEX_WAIT] = sys_futex_wait,
    [OS_SYS_FUTEX_WAKE] = sys_futex_wake,
    [OS_SYS_FILE_OPEN] = sys_file_open,
    [OS_SYS_FILE_ENUMERATE] = sys_file_enumerate,
    [OS_SYS_FILE_SEEK] = sys_file_seek,
    [OS_SYS_FILE_STAT] = sys_file_stat,
    [OS_SYS_FILE_TRUNCATE] = sys_file_truncate,
    [OS_SYS_FILE_REMOVE] = sys_file_remove,
    [OS_SYS_FILE_MKDIR] = sys_file_mkdir,
    [OS_SYS_FILE_READ] = sys_file_read,
    [OS_SYS_FILE_WRITE] = sys_file_write,
    [OS_SYS_FILE_FSYNC] = sys_file_fsync,
    [OS_SYS_IO_SUBMIT] = sys_io_submit,
    [OS_SYS_IO_CANCEL] = sys_io_cancel,
    [OS_SYS_SOCKET_CREATE] = sys_socket_create,
    [OS_SYS_SOCKET_BIND] = sys_socket_bind,
    [OS_SYS_SOCKET_CONNECT] = sys_socket_connect,
    [OS_SYS_SOCKET_LISTEN] = sys_socket_listen,
    [OS_SYS_SOCKET_ACCEPT] = sys_socket_accept,
    [OS_SYS_SOCKET_SEND] = sys_socket_send,
    [OS_SYS_SOCKET_RECV] = sys_socket_recv,
    [OS_SYS_SOCKET_SEND_ASYNC] = sys_socket_send_async,
    [OS_SYS_SOCKET_BIND6] = sys_socket_bind6,
    [OS_SYS_SOCKET_CONNECT6] = sys_socket_connect6,
    [OS_SYS_SOCKET_SEND6] = sys_socket_send6,
    [OS_SYS_SOCKET_RECV6] = sys_socket_recv6,
    [OS_SYS_SOCKET_SEND_ASYNC6] = sys_socket_send_async6,
    [OS_SYS_NET_GET_STATUS] = sys_net_get_status,
    [OS_SYS_NET_SET_IPV4] = sys_net_set_ipv4,
    [OS_SYS_NET_SUBSCRIBE] = sys_net_subscribe,
    [OS_SYS_GPU_CREATE_CTX] = sys_gpu_create_context,
    [OS_SYS_GPU_ALLOC] = sys_gpu_alloc,
    [OS_SYS_GPU_MAP] = sys_gpu_map,
    [OS_SYS_GPU_SUBMIT] = sys_gpu_submit,
    [OS_SYS_GPU_WAIT_FENCE] = sys_gpu_wait_fence,
    [OS_SYS_DISPLAY_GET_INFO] = sys_display_get_info,
    [OS_SYS_DISPLAY_COMMIT] = sys_display_commit,
    [OS_SYS_INPUT_READ] = sys_input_read,
    [OS_SYS_WINDOW_REGISTER_MANAGER] = sys_window_register_manager,
    [OS_SYS_WINDOW_CREATE] = sys_window_create,
    [OS_SYS_WINDOW_ENUMERATE] = sys_window_enumerate,
    [OS_SYS_WINDOW_MAP] = sys_window_map,
    [OS_SYS_WINDOW_SET] = sys_window_set,
    [OS_SYS_WINDOW_FOCUS] = sys_window_focus,
    [OS_SYS_WINDOW_INPUT_READ] = sys_window_input_read,
    [OS_SYS_WINDOW_INPUT_DISPATCH] = sys_window_input_dispatch,
    [OS_SYS_WINDOW_EVENT_READ] = sys_window_event_read,
    [OS_SYS_WINDOW_UPDATE] = sys_window_update,
    [OS_SYS_AUDIO_OPEN] = sys_audio_open,
    [OS_SYS_AUDIO_CONTROL] = sys_audio_control,
    [OS_SYS_TOKEN_QUERY] = sys_token_query,
    [OS_SYS_DEBUG_QUERY] = sys_debug_query,
};

BOOLEAN liteos_syscall_init(uint64_t kernel_stack_top) {
    if (kernel_stack_top == 0 || (kernel_stack_top & 0xFULL) != 0) return 0;
    LITEOS_SYSCALL_CPU_LOCAL *local = x86_cpu_local_current();
    if (local == 0) return 0;
    local->UserStack = 0;
    local->KernelStack = kernel_stack_top;
    local->KernelResumeStack = 0;
    local->ReturnToKernel = 0;
    local->UserExitSeen = 0;

    /*
     * SYSRET 在长模式下用 STAR 高半值加 8/16 装载 SS/CS。
     * GDT 的用户数据段基址为 0x18、用户代码段基址为 0x20，
     * 因而 STAR 高半值必须为 0x13，才能得到带 RPL3 的
     * SS=0x1B、CS=0x23。使用 0x10 会产生 SS=0x18，之后的
     * 定时器 iretq 会把它视为错误的返回帧。
     */
    write_msr(IA32_STAR, (0x13ULL << 48) | (0x08ULL << 32));
    write_msr(IA32_LSTAR, (uint64_t)(uintptr_t)&liteos_syscall_entry);
    write_msr(IA32_FMASK, RFLAGS_DIRECTION | RFLAGS_INTERRUPT | RFLAGS_TRAP |
                             RFLAGS_ALIGNMENT_CHECK | RFLAGS_NESTED_TASK);
    /* 内核态始终使用 CPU-local GS；swapgs 后用户态得到零基址。 */
    write_msr(IA32_GS_BASE, (uint64_t)(uintptr_t)local);
    write_msr(IA32_KERNEL_GS_BASE, 0);
    write_msr(IA32_EFER, read_msr(IA32_EFER) | EFER_SYSCALL_ENABLE);
    return 1;
}

void x86_syscall_set_kernel_stack(uint64_t kernel_stack_top) {
    if (kernel_stack_top != 0 && (kernel_stack_top & 15U) == 0) {
        LITEOS_SYSCALL_CPU_LOCAL *local = x86_cpu_local_current();
        if (local != 0) local->KernelStack = kernel_stack_top;
    }
}

void x86_syscall_init(void) {
    uint64_t stack;
    __asm__ volatile ("mov %%rsp, %0" : "=r"(stack));
    (void)liteos_syscall_init(stack & ~0xFULL);
}

int64_t liteos_syscall_dispatch(arch_trap_frame_t *frame) {
    thread_t *executing_thread;
    if (frame == 0 || !user_address_valid(frame->rip) ||
        !user_address_valid(frame->rsp)) return K_EACCES;
    executing_thread = sched_current_thread();
    /* 从硬中断排队的设备工作在普通内核上下文执行，严格限制本次预算。 */
    (void)deferred_run(4U);
    uint64_t number = frame->rax;
    if (number >= SYSCALL_TABLE_SIZE || g_syscall_table[number] == 0) return K_ENOSYS;
    int64_t status = g_syscall_table[number](frame->rdi, frame->rsi, frame->rdx,
                                             frame->r10, frame->r8, frame->r9);
    if (status == K_OK) {
        if (executing_thread != 0 && executing_thread->exec_pending) {
            frame->rip = executing_thread->exec_entry;
            frame->rsp = executing_thread->exec_stack;
            frame->rflags = 0x202ULL;
            frame->rax = 0;
            executing_thread->exec_pending = false;
        }
    }
    return status;
}

uint32_t liteos_syscall_thread_create_stage(void) {
    return g_thread_create_stage;
}

bool x86_validate_user_frame(const arch_trap_frame_t *frame) {
    if (frame == 0 || !user_address_valid(frame->rip) ||
        !user_address_valid(frame->rsp)) return false;
    if (frame->cs != USER_CODE_SELECTOR || frame->ss != USER_DATA_SELECTOR) return false;
    if ((frame->rflags & ~RFLAGS_USER_ALLOWED) != 0) return false;
    if ((frame->rflags & RFLAGS_FIXED) == 0) return false;
    if ((frame->rflags & (RFLAGS_IOPL | RFLAGS_NESTED_TASK | RFLAGS_VIRTUAL_8086)) != 0) {
        return false;
    }
    return true;
}

int x86_syscall_return_mode(arch_trap_frame_t *frame) {
    if (!x86_validate_user_frame(frame)) return -1;
    /* 调试/恢复/对齐检查等标志走 IRETQ，常规返回走 SYSRETQ。 */
    uint64_t slow_flags = RFLAGS_TRAP | RFLAGS_RESUME | RFLAGS_ALIGNMENT_CHECK;
    return (frame->rflags & slow_flags) == 0 ? 1 : 0;
}

__noreturn void x86_syscall_bad_frame(void) {
    thread_t *thread = sched_current_thread();
    /* 用户返回帧损坏时只终止当前线程，不能让一个进程拖垮整个内核。 */
    if (thread != 0 && thread->object.type == KOBJECT_TYPE_THREAD &&
        thread->process != 0) {
        thread_exit(K_EACCES);
    }
    /* 早期启动阶段没有可回收的用户线程，只能进入不可恢复停机。 */
    for (;;) __asm__ volatile ("cli; hlt" : : : "memory");
}

bool syscall_frame_self_test(void) {
    arch_trap_frame_t frame = {0};
    uint64_t seed = 0xC0DEC0DE12345678ULL;
    frame.rip = 0x0000000040000000ULL;
    frame.rsp = 0x0000000080000000ULL;
    frame.cs = USER_CODE_SELECTOR;
    frame.ss = USER_DATA_SELECTOR;
    frame.rflags = RFLAGS_FIXED | RFLAGS_INTERRUPT;
    frame.rax = OS_SYS_DEBUG_QUERY;
    if (!x86_validate_user_frame(&frame) || x86_syscall_return_mode(&frame) != 1 ||
        liteos_syscall_dispatch(&frame) != OS_SYSCALL_ABI_VERSION) return false;

    frame.rflags |= RFLAGS_TRAP;
    if (x86_syscall_return_mode(&frame) != 0) return false;
    frame.rflags &= ~RFLAGS_TRAP;
    frame.rax = SYSCALL_TABLE_SIZE;
    if (liteos_syscall_dispatch(&frame) != K_ENOSYS) return false;

    frame.rip = 0x0000800000000000ULL;
    if (x86_validate_user_frame(&frame)) return false;
    frame.rip = 0x0000000040000000ULL;
    frame.cs = 0x08U;
    if (x86_validate_user_frame(&frame)) return false;
    frame.cs = USER_CODE_SELECTOR;
    frame.rflags |= RFLAGS_IOPL;
    if (x86_validate_user_frame(&frame)) return false;
    frame.rflags = RFLAGS_INTERRUPT;
    if (x86_validate_user_frame(&frame)) return false;

    /* 确定性扰动返回帧，覆盖常见的用户态伪造输入而不执行任意 syscall。 */
    for (uint32_t i = 0; i < 256U; ++i) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        frame.rip = 0x0000800000000000ULL | (seed & 0xFFFFU);
        frame.rsp = 0x0000000080000000ULL;
        frame.cs = USER_CODE_SELECTOR;
        frame.ss = USER_DATA_SELECTOR;
        frame.rflags = RFLAGS_FIXED | RFLAGS_INTERRUPT;
        if (x86_validate_user_frame(&frame) ||
            liteos_syscall_dispatch(&frame) != K_EACCES) return false;

        frame.rip = 0x0000000040000000ULL;
        frame.rsp = 0x0000800000000000ULL | ((seed >> 16) & 0xFFFFU);
        if (x86_validate_user_frame(&frame) ||
            liteos_syscall_dispatch(&frame) != K_EACCES) return false;

        frame.rsp = 0x0000000080000000ULL;
        frame.cs = (uint16_t)(((seed | 1U) ^ USER_CODE_SELECTOR) | 0x0040U);
        if (x86_validate_user_frame(&frame)) return false;
        frame.cs = USER_CODE_SELECTOR;
        frame.ss = (uint16_t)(((seed >> 32) ^ USER_DATA_SELECTOR) | 0x0080U);
        if (x86_validate_user_frame(&frame)) return false;
        frame.ss = USER_DATA_SELECTOR;
        frame.rflags = RFLAGS_FIXED | RFLAGS_IOPL;
        if (x86_validate_user_frame(&frame)) return false;

        /* 每个保留位都必须被拒绝，避免伪造帧进入 SYSRETQ/IRETQ。 */
        frame.rflags = RFLAGS_FIXED | RFLAGS_INTERRUPT;
        uint64_t flag_bit = 1ULL;
        for (uint32_t bit = 0; bit < 64U; ++bit, flag_bit <<= 1U) {
            if ((RFLAGS_USER_ALLOWED & flag_bit) != 0) continue;
            frame.rflags = RFLAGS_FIXED | RFLAGS_INTERRUPT | flag_bit;
            if (x86_validate_user_frame(&frame)) return false;
        }

        /* 访问范围的两个边界及高地址随机值都必须保持为无效帧。 */
        frame.rflags = RFLAGS_FIXED | RFLAGS_INTERRUPT;
        frame.rip = USER_ADDRESS_MIN - 1U;
        if (x86_validate_user_frame(&frame)) return false;
        frame.rip = USER_ADDRESS_END;
        if (x86_validate_user_frame(&frame)) return false;
        frame.rip = 0xFFFFFFFFFFFFFFFFULL;
        if (x86_validate_user_frame(&frame)) return false;
        frame.rip = 0x0000000040000000ULL;
        frame.rsp = USER_ADDRESS_MIN - 1U;
        if (x86_validate_user_frame(&frame)) return false;
        frame.rsp = USER_ADDRESS_END;
        if (x86_validate_user_frame(&frame)) return false;
    }
    return true;
}
