#include <arch/x86_64/syscall_internal.h>
#include <arch/x86_64/paging.h>
#include <arch/x86_64/uaccess.h>
#include <kernel/futex.h>
#include <kernel/console.h>
#include <kernel/debug_stage.h>
#include <kernel/completion_port.h>
#include <kernel/message_port.h>
#include <kernel/timer.h>
#include <kernel/deferred.h>
#include <kernel/mm.h>
#include <kernel/kmem.h>
#include <kernel/vm.h>
#include <kernel/shared_section.h>
#include <kernel/process.h>
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

#include "internal.h"




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

syscall_cpu_local_t liteos_syscall_cpu_local;
volatile uint32_t g_syscall_return_progress[MAX_CPUS];
volatile uint64_t g_syscall_return_number[MAX_CPUS];
volatile uint64_t g_syscall_return_rip[MAX_CPUS];
volatile uint32_t g_socket_return_progress[MAX_CPUS];
volatile uint32_t g_socket_return_active[MAX_CPUS];
volatile uint64_t g_socket_return_rip[MAX_CPUS];

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

bool versioned_header_valid(const os_versioned_header_t *header,
                            size_t required_size) {
    return header != 0 && header->size >= required_size &&
           header->version == OS_SYSCALL_ABI_VERSION && header->flags == 0;
}

process_t *current_process(void) {
    thread_t *thread = sched_current_thread();
    if (thread == 0 || thread->object.type != KOBJECT_TYPE_THREAD ||
        thread->process == 0) return 0;
    return thread->process;
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
    [OS_SYS_THREAD_EXIT] = syscall_thread_exit,
    [OS_SYS_PROCESS_EXIT] = syscall_process_exit,
    [OS_SYS_THREAD_CREATE] = syscall_thread_create,
    [OS_SYS_PROCESS_CREATE] = syscall_process_create,
    [OS_SYS_PROCESS_EXEC] = syscall_process_exec,
    [OS_SYS_PROCESS_INFO] = syscall_process_info,
    [OS_SYS_PROCESS_FORK] = syscall_process_fork,
    [OS_SYS_PROCESS_WAIT] = syscall_process_wait,
    [OS_SYS_PROCESS_ENUMERATE] = syscall_process_enumerate,
    [OS_SYS_THREAD_ENUMERATE] = syscall_thread_enumerate,
    [OS_SYS_THREAD_CONTEXT] = syscall_thread_context,
    [OS_SYS_VM_MAP] = syscall_vm_map,
    [OS_SYS_VM_UNMAP] = syscall_vm_unmap,
    [OS_SYS_VM_PROTECT] = syscall_vm_protect,
    [OS_SYS_VM_SHARE] = syscall_vm_share,
    [OS_SYS_VM_SYNC] = syscall_vm_sync,
    [OS_SYS_VM_ADVISE] = syscall_vm_advise,
    [OS_SYS_HANDLE_CLOSE] = syscall_handle_close,
    [OS_SYS_HANDLE_DUP] = syscall_handle_dup,
    [OS_SYS_HANDLE_GET_FLAGS] = syscall_handle_get_flags,
    [OS_SYS_HANDLE_SET_FLAGS] = syscall_handle_set_flags,
    [OS_SYS_DEVICE_OPEN] = syscall_device_open,
    [OS_SYS_DEVICE_CONTROL] = syscall_device_control,
    [OS_SYS_DEVICE_ENUMERATE] = syscall_device_enumerate,
    [OS_SYS_PORT_CREATE] = syscall_port_create,
    [OS_SYS_PORT_SEND] = syscall_port_send,
    [OS_SYS_PORT_RECEIVE] = syscall_port_receive,
    [OS_SYS_COMPLETION_WAIT] = syscall_completion_wait,
    [OS_SYS_CLOCK_GET] = syscall_clock_get,
    [OS_SYS_TIMER_CREATE] = syscall_timer_create,
    [OS_SYS_CLOCK_SET] = syscall_clock_set,
    [OS_SYS_RANDOM_GET] = syscall_random_get,
    [OS_SYS_EXCEPTION_RETURN] = syscall_signal_return,
    [OS_SYS_SIGNAL_ACTION] = syscall_signal_action,
    [OS_SYS_SIGNAL_MASK] = syscall_signal_mask,
    [OS_SYS_SIGNAL_SEND] = syscall_signal_send,
    [OS_SYS_WAIT_ONE] = syscall_wait_one,
    [OS_SYS_WAIT_MANY] = syscall_wait_many,
    [OS_SYS_FUTEX_WAIT] = syscall_futex_wait,
    [OS_SYS_FUTEX_WAKE] = syscall_futex_wake,
    [OS_SYS_FILE_OPEN] = syscall_file_open,
    [OS_SYS_FILE_ENUMERATE] = syscall_file_enumerate,
    [OS_SYS_FILE_SEEK] = syscall_file_seek,
    [OS_SYS_FILE_STAT] = syscall_file_stat,
    [OS_SYS_FILE_TRUNCATE] = syscall_file_truncate,
    [OS_SYS_FILE_REMOVE] = syscall_file_remove,
    [OS_SYS_FILE_MKDIR] = syscall_file_mkdir,
    [OS_SYS_FILE_FSTAT] = syscall_file_fstat,
    [OS_SYS_FILE_RENAME] = syscall_file_rename,
    [OS_SYS_FILE_READ] = syscall_file_read,
    [OS_SYS_FILE_WRITE] = syscall_file_write,
    [OS_SYS_FILE_FSYNC] = syscall_file_fsync,
    [OS_SYS_PIPE_CREATE] = syscall_pipe_create,
    [OS_SYS_PIPE_READ] = syscall_pipe_read,
    [OS_SYS_PIPE_WRITE] = syscall_pipe_write,
    [OS_SYS_IO_SUBMIT] = syscall_io_submit,
    [OS_SYS_IO_CANCEL] = syscall_io_cancel,
    [OS_SYS_SOCKET_CREATE] = syscall_socket_create,
    [OS_SYS_SOCKET_BIND] = syscall_socket_bind,
    [OS_SYS_SOCKET_CONNECT] = syscall_socket_connect,
    [OS_SYS_SOCKET_LISTEN] = syscall_socket_listen,
    [OS_SYS_SOCKET_ACCEPT] = syscall_socket_accept,
    [OS_SYS_SOCKET_SEND] = syscall_socket_send,
    [OS_SYS_SOCKET_RECV] = syscall_socket_recv,
    [OS_SYS_SOCKET_SEND_ASYNC] = syscall_socket_send_async,
    [OS_SYS_SOCKET_BIND6] = syscall_socket_bind6,
    [OS_SYS_SOCKET_CONNECT6] = syscall_socket_connect6,
    [OS_SYS_SOCKET_SEND6] = syscall_socket_send6,
    [OS_SYS_SOCKET_RECV6] = syscall_socket_recv6,
    [OS_SYS_SOCKET_SEND_ASYNC6] = syscall_socket_send_async6,
    [OS_SYS_SOCKET_GET_INFO] = syscall_socket_get_info,
    [OS_SYS_SOCKET_GET_OPTION] = syscall_socket_get_option,
    [OS_SYS_SOCKET_SET_OPTION] = syscall_socket_set_option,
    [OS_SYS_SOCKET_SHUTDOWN] = syscall_socket_shutdown,
    [OS_SYS_NET_GET_STATUS] = syscall_net_get_status,
    [OS_SYS_NET_SET_IPV4] = syscall_net_set_ipv4,
    [OS_SYS_NET_SUBSCRIBE] = syscall_net_subscribe,
    [OS_SYS_GPU_CREATE_CTX] = syscall_gpu_create_context,
    [OS_SYS_GPU_ALLOC] = syscall_gpu_alloc,
    [OS_SYS_GPU_MAP] = syscall_gpu_map,
    [OS_SYS_GPU_SUBMIT] = syscall_gpu_submit,
    [OS_SYS_GPU_WAIT_FENCE] = syscall_gpu_wait_fence,
    [OS_SYS_DISPLAY_GET_INFO] = syscall_display_get_info,
    [OS_SYS_DISPLAY_COMMIT] = syscall_display_commit,
    [OS_SYS_FONT_CACHE] = syscall_font_cache,
    [OS_SYS_IMAGE_INFO] = syscall_image_info,
    [OS_SYS_IMAGE_DECODE] = syscall_image_decode,
    [OS_SYS_INPUT_READ] = syscall_input_read,
    [OS_SYS_WINDOW_REGISTER_MANAGER] = syscall_window_register_manager,
    [OS_SYS_WINDOW_CREATE] = syscall_window_create,
    [OS_SYS_WINDOW_ENUMERATE] = syscall_window_enumerate,
    [OS_SYS_WINDOW_MAP] = syscall_window_map,
    [OS_SYS_WINDOW_SET] = syscall_window_set,
    [OS_SYS_WINDOW_FOCUS] = syscall_window_focus,
    [OS_SYS_WINDOW_INPUT_READ] = syscall_window_input_read,
    [OS_SYS_WINDOW_INPUT_DISPATCH] = syscall_window_input_dispatch,
    [OS_SYS_WINDOW_EVENT_READ] = syscall_window_event_read,
    [OS_SYS_WINDOW_UPDATE] = syscall_window_update,
    [OS_SYS_AUDIO_OPEN] = syscall_audio_open,
    [OS_SYS_AUDIO_CONTROL] = syscall_audio_control,
    [OS_SYS_DEBUG_QUERY] = sys_debug_query,
    [OS_SYS_DEBUG_WRITE] = syscall_debug_write,
};

bool liteos_syscall_init(uint64_t kernel_stack_top) {
    if (kernel_stack_top == 0 || (kernel_stack_top & 0xFULL) != 0) return 0;
    syscall_cpu_local_t *local = x86_cpu_local_current();
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
        syscall_cpu_local_t *local = x86_cpu_local_current();
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
    uint64_t argument5 = frame->r9;
    /* PROCESS_FORK has no user argument; its private sixth argument carries
     * the kernel-owned return frame so the child can resume after syscall. */
    if (number == OS_SYS_PROCESS_FORK || number == OS_SYS_SIGNAL_RETURN) {
        argument5 = (uint64_t)(uintptr_t)frame;
    }
    int64_t status = g_syscall_table[number](frame->rdi, frame->rsi, frame->rdx,
                                             frame->r10, frame->r8, argument5);
    if (status == K_OK) {
        if (executing_thread != 0 && executing_thread->exec_pending) {
            process_exec_debug_mark(11U);
            frame->rip = executing_thread->exec_entry;
            frame->rsp = executing_thread->exec_stack;
            frame->rflags = 0x202ULL;
            frame->rax = 0;
            executing_thread->exec_pending = false;
            process_exec_debug_mark(12U);
        }
    }
    if (number != OS_SYS_SIGNAL_RETURN) {
        status = syscall_deliver_pending_signal(frame, status);
    }
    return status;
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

uint32_t x86_syscall_return_progress(uint32_t cpu_index) {
    if (cpu_index >= MAX_CPUS) return 0U;
    return __atomic_load_n(&g_syscall_return_progress[cpu_index], __ATOMIC_ACQUIRE);
}

uint64_t x86_syscall_return_number(uint32_t cpu_index) {
    if (cpu_index >= MAX_CPUS) return 0U;
    return __atomic_load_n(&g_syscall_return_number[cpu_index], __ATOMIC_ACQUIRE);
}

uint64_t x86_syscall_return_rip(uint32_t cpu_index) {
    if (cpu_index >= MAX_CPUS) return 0U;
    return __atomic_load_n(&g_syscall_return_rip[cpu_index], __ATOMIC_ACQUIRE);
}

uint32_t x86_socket_return_progress(uint32_t cpu_index) {
    if (cpu_index >= MAX_CPUS) return 0U;
    return __atomic_load_n(&g_socket_return_progress[cpu_index], __ATOMIC_ACQUIRE);
}

uint32_t x86_socket_return_active(uint32_t cpu_index) {
    if (cpu_index >= MAX_CPUS) return 0U;
    return __atomic_load_n(&g_socket_return_active[cpu_index], __ATOMIC_ACQUIRE);
}

uint64_t x86_socket_return_rip(uint32_t cpu_index) {
    if (cpu_index >= MAX_CPUS) return 0U;
    return __atomic_load_n(&g_socket_return_rip[cpu_index], __ATOMIC_ACQUIRE);
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
