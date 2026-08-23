#include <arch/x86_64/cpu.h>
#include <arch/x86_64/paging.h>
#include <kernel/device.h>
#include <kernel/elf_loader.h>
#include <kernel/kmem.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/vfs.h>
#include <uapi/device.h>
#include <uapi/audio.h>
#include <uapi/process.h>
#include <uapi/syscall.h>

#define INIT_IMAGE_SIZE       0x3000U
#define INIT_DATA_ADDRESS     0x00401000ULL
#define INIT_SERVICE_COUNT     6U
#define INIT_DEVICE_ID         0x4C4954454F530001ULL
#define INIT_DEVICE_CLASS      0x0600U
#define INIT_RUNTIME_RESTART_LIMIT 3U

#define INIT_BOOTSTRAP_EXIT_TIMEOUT_NS 15000000000ULL

static uint32_t g_user_init_stage;
static int64_t g_user_init_result;
static device_t g_user_init_device;
static bool g_user_init_device_registered;
static process_t *g_runtime_init_process;
static thread_t *g_runtime_init_thread;
static bool g_runtime_init_started;
static uint32_t g_runtime_init_restart_count;

static kstatus_t user_init_device_reset(device_t *device, uint32_t level) {
    return device != 0 && level < 3U ? K_OK : K_EINVAL;
}

static const device_ops_t g_user_init_device_ops = {
    .reset = user_init_device_reset,
};

static bool init_register_device(void) {
    device_object_init(&g_user_init_device, INIT_DEVICE_ID, INIT_DEVICE_CLASS,
                       &g_user_init_device_ops, 0);
    if (device_register(&g_user_init_device) != K_OK) return false;
    g_user_init_device_registered = true;
    return true;
}

#if 0
static void init_zero(void *target, size_t size) {
    uint8_t *bytes = (uint8_t *)target;
    for (size_t i = 0; i < size; ++i) bytes[i] = 0;
}

static bool init_make_image(uint8_t *image, bool persistent, size_t *image_size) {
    if (image == 0 || image_size == 0) return false;
    init_zero(image, INIT_IMAGE_SIZE);
    size_t code_size = (size_t)(liteos_init_blob_end - liteos_init_blob_start);
    if (code_size == 0 || code_size > PAGE_SIZE_LOCAL) return false;

    init_elf64_header_t *header = (init_elf64_header_t *)image;
    header->ident[0] = 0x7FU;
    header->ident[1] = 'E';
    header->ident[2] = 'L';
    header->ident[3] = 'F';
    header->ident[4] = ELF_CLASS_64;
    header->ident[5] = ELF_DATA_LSB;
    header->ident[6] = ELF_VERSION_CURRENT;
    header->type = ELF_TYPE_EXEC;
    header->machine = ELF_MACHINE_X86_64;
    header->version = ELF_VERSION_CURRENT;
    header->entry = INIT_CODE_ADDRESS;
    header->program_header_offset = sizeof(*header);
    header->header_size = sizeof(*header);
    header->program_header_size = sizeof(init_elf64_program_header_t);
    header->program_header_count = 2U;

    init_elf64_program_header_t *programs =
        (init_elf64_program_header_t *)(image + sizeof(*header));
    programs[0].type = ELF_PT_LOAD;
    programs[0].flags = ELF_PF_READ | ELF_PF_EXEC;
    programs[0].offset = INIT_CODE_OFFSET;
    programs[0].virtual_address = INIT_CODE_ADDRESS;
    programs[0].file_size = code_size;
    programs[0].memory_size = PAGE_SIZE_LOCAL;
    programs[0].alignment = PAGE_SIZE_LOCAL;

    programs[1].type = ELF_PT_LOAD;
    programs[1].flags = ELF_PF_READ | ELF_PF_WRITE;
    programs[1].offset = INIT_DATA_OFFSET;
    programs[1].virtual_address = INIT_DATA_ADDRESS;
    programs[1].file_size = INIT_DATA_FILE_SIZE;
    programs[1].memory_size = PAGE_SIZE_LOCAL;
    programs[1].alignment = PAGE_SIZE_LOCAL;

    for (size_t i = 0; i < code_size; ++i) {
        image[INIT_CODE_OFFSET + i] = liteos_init_blob_start[i];
    }

    image[INIT_DATA_OFFSET + INIT_PERSISTENT_OFFSET] = persistent ? 1U : 0U;
    *(uint32_t *)(void *)(image + INIT_DATA_OFFSET + INIT_PERSISTENT_WORD_OFFSET) =
        persistent ? 1U : 0U;

    os_thread_create_t *thread_arguments =
        (os_thread_create_t *)(image + INIT_DATA_OFFSET + 0x50U);
    thread_arguments->hdr.size = sizeof(*thread_arguments);
    thread_arguments->hdr.version = OS_SYSCALL_ABI_VERSION;
    thread_arguments->entry = INIT_CODE_ADDRESS +
        (uint64_t)(liteos_init_service - liteos_init_blob_start);
    thread_arguments->stack_top = 0;
    thread_arguments->fs_base = 0;
    thread_arguments->argument = 0;
    thread_arguments->flags = 0;
    thread_arguments->reserved = 0;

    os_device_open_t *device_open =
        (os_device_open_t *)(image + INIT_DATA_OFFSET + INIT_DEVICE_OPEN_OFFSET);
    device_open->hdr.size = sizeof(*device_open);
    device_open->hdr.version = OS_SYSCALL_ABI_VERSION;
    device_open->device_id = INIT_DEVICE_ID;
    device_open->desired_rights = OS_DEVICE_RIGHT_ALL;

    os_device_control_t *device_control =
        (os_device_control_t *)(image + INIT_DATA_OFFSET +
                                INIT_DEVICE_CONTROL_OFFSET);
    device_control->hdr.size = sizeof(*device_control);
    device_control->hdr.version = OS_SYSCALL_ABI_VERSION;
    device_control->code = OS_DEVICE_CONTROL_QUERY;
    device_control->output = INIT_DATA_ADDRESS + INIT_DEVICE_INFO_OFFSET;
    device_control->output_size = sizeof(os_device_info_t);

    os_device_info_t *device_info =
        (os_device_info_t *)(image + INIT_DATA_OFFSET + INIT_DEVICE_INFO_OFFSET);
    device_info->hdr.size = sizeof(*device_info);
    device_info->hdr.version = OS_SYSCALL_ABI_VERSION;

    /* deviced 先通过公开枚举接口发现设备，再按稳定 ID 打开目标设备。 */
    os_device_enumerate_t *device_enumerate =
        (os_device_enumerate_t *)(image + INIT_DATA_OFFSET +
                                  INIT_DEVICE_ENUMERATE_OFFSET);
    device_enumerate->hdr.size = sizeof(*device_enumerate);
    device_enumerate->hdr.version = OS_SYSCALL_ABI_VERSION;
    device_enumerate->index = 0U;
    device_enumerate->output = INIT_DATA_ADDRESS + INIT_DEVICE_INFO_OFFSET;
    device_enumerate->output_size = sizeof(os_device_info_t);

    static const char audio_path[] = "/sbin/audiod";
    for (size_t i = 0; i < sizeof(audio_path); ++i) {
        image[INIT_DATA_OFFSET + INIT_AUDIO_EXEC_PATH_OFFSET + i] =
            (uint8_t)audio_path[i];
    }

    static const char shell_path[] = "/sbin/gshell";
    for (size_t i = 0; i < sizeof(shell_path); ++i) {
        image[INIT_DATA_OFFSET + INIT_SHELL_PATH_OFFSET + i] =
            (uint8_t)shell_path[i];
    }
    static const char netmgr_path[] = "/sbin/netd";
    for (size_t i = 0; i < sizeof(netmgr_path); ++i) {
        image[INIT_DATA_OFFSET + INIT_NETMGR_PATH_OFFSET + i] =
            (uint8_t)netmgr_path[i];
    }
    os_audio_open_t *audio_open =
        (os_audio_open_t *)(image + INIT_DATA_OFFSET + INIT_AUDIO_OPEN_OFFSET);
    audio_open->hdr.size = sizeof(*audio_open);
    audio_open->hdr.version = OS_SYSCALL_ABI_VERSION;
    audio_open->config.hdr.size = sizeof(audio_open->config);
    audio_open->config.hdr.version = OS_SYSCALL_ABI_VERSION;
    audio_open->config.direction = OS_AUDIO_PLAYBACK;
    audio_open->config.sample_rate = 48000U;
    audio_open->config.channels = 2U;
    audio_open->config.sample_format = OS_AUDIO_SAMPLE_S16_LE;
    audio_open->config.period_frames = 128U;
    audio_open->config.period_count = 2U;

    os_audio_control_t *audio_control =
        (os_audio_control_t *)(image + INIT_DATA_OFFSET +
                               INIT_AUDIO_CONTROL_OFFSET);
    audio_control->hdr.size = sizeof(*audio_control);
    audio_control->hdr.version = OS_SYSCALL_ABI_VERSION;

    os_audio_stream_stats_t *audio_stats =
        (os_audio_stream_stats_t *)(image + INIT_DATA_OFFSET +
                                    INIT_AUDIO_STATS_OFFSET);
    audio_stats->hdr.size = sizeof(*audio_stats);
    audio_stats->hdr.version = OS_SYSCALL_ABI_VERSION;

    *image_size = INIT_IMAGE_SIZE;
    return true;
}
#endif

static kstatus_t init_load_path(const char *path, process_t *process,
                                user_elf_image_info_t *info,
                                vaddr_t *stack_pointer) {
    file_t *file = 0;
    kstatus_t status = vfs_open_kernel(path, VFS_OPEN_READ, &file);
    if (status != K_OK) return status;
    if (file->vnode == 0 || file->vnode->size == 0 ||
        file->vnode->size > INIT_IMAGE_SIZE) {
        vfs_close(file);
        return K_EINVAL;
    }
    size_t image_size = (size_t)file->vnode->size;
    uint8_t *image = (uint8_t *)kmalloc(image_size, 0);
    if (image == 0) {
        vfs_close(file);
        return K_ENOMEM;
    }
    uint64_t bytes = 0;
    status = vfs_read_kernel(file, image, image_size, &bytes);
    vfs_close(file);
    if (status == K_OK && bytes != image_size) status = K_EIO;
    if (status == K_OK) {
        status = process_load_elf_image(process, image, image_size,
                                         info, stack_pointer);
    }
    kfree(image);
    return status;
}

static bool init_wait_for_exit(process_t *process, thread_t *thread,
                               vm_space_t *runtime_space) {
    paddr_t marker_physical;
    uint8_t stage = 0;
    uint32_t service_count = 0;
    if (x86_translate_page(runtime_space->root_table, INIT_DATA_ADDRESS,
                           &marker_physical, 0) == K_OK) {
        uint8_t *marker_page = (uint8_t *)phys_to_direct(marker_physical);
        if (marker_page != 0) {
            stage = marker_page[0];
            service_count = *(const uint32_t *)(const void *)(marker_page + 0x10U);
            g_user_init_result = *(const int64_t *)(const void *)(marker_page + 0x08U);
        }
    }
    g_user_init_stage = stage;

    uint64_t deadline = x86_read_tsc();
    uint64_t budget =
        x86_timeout_ns_to_tsc(INIT_BOOTSTRAP_EXIT_TIMEOUT_NS);
    deadline = budget > UINT64_MAX - deadline ? UINT64_MAX : deadline + budget;
    for (;;) {
        __asm__ volatile ("sti" : : : "memory");
        schedule();
        sched_finish_switch();
        if (atomic_load_explicit(&thread->state, memory_order_acquire) == THREAD_DEAD &&
            atomic_load_explicit(&process->state, memory_order_acquire) == PROCESS_DEAD) {
            break;
        }
        if ((int64_t)(x86_read_tsc() - deadline) >= 0) break;
        __asm__ volatile ("pause");
    }

    /* 用户线程可能在关中断状态下退出，给调度器几次机会完成 execution ref 回收。 */
    __asm__ volatile ("sti" : : : "memory");
    for (uint32_t attempt = 0; attempt < 4U && process->vm != 0; ++attempt) {
        sched_finish_switch();
        if (process->vm == 0) break;
        schedule();
    }
    /* 若退出发生在 syscall 的关中断窗口，调度器会延后 reaper；这里安全地补做一次幂等释放。 */
    thread_release_execution_ref(thread);

    if (x86_translate_page(runtime_space->root_table, INIT_DATA_ADDRESS,
                           &marker_physical, 0) == K_OK) {
        uint8_t *marker_page = (uint8_t *)phys_to_direct(marker_physical);
        if (marker_page != 0) {
            stage = marker_page[0];
            service_count = *(const uint32_t *)(const void *)(marker_page + 0x10U);
            g_user_init_result = *(const int64_t *)(const void *)(marker_page + 0x08U);
        }
    }
    g_user_init_stage = stage;
    if (g_user_init_result == 0 && thread->exit_code != 0) {
        g_user_init_result = thread->exit_code;
    }
    bool thread_dead = atomic_load_explicit(&thread->state, memory_order_acquire) == THREAD_DEAD;
    bool process_dead = atomic_load_explicit(&process->state, memory_order_acquire) == PROCESS_DEAD;
    bool success = thread_dead && thread->exit_code == 0 && process->thread_count == 0 &&
                   process_dead && process->exit_code == 0 && process->vm == 0 &&
                   stage == 0xA5U && service_count == INIT_SERVICE_COUNT &&
                   g_user_init_result == 0;
    if (!success && g_user_init_result == 0) {
        g_user_init_result = (int64_t)((thread_dead ? 0U : 1U) |
            (thread->exit_code == 0 ? 0U : 2U) |
            (process->thread_count == 0 ? 0U : 4U) |
            (process_dead ? 0U : 8U) |
            (process->exit_code == 0 ? 0U : 16U) |
            (process->vm == 0 ? 0U : 32U) |
            (stage == 0xA5U ? 0U : 64U) |
            (service_count == INIT_SERVICE_COUNT ? 0U : 128U));
    }
    return success;
}

bool user_init_bootstrap_self_test(void) {
    process_t *process = 0;
    thread_t *thread = 0;
    vm_space_t *runtime_space = 0;
    user_elf_image_info_t info;
    vaddr_t stack_pointer = 0;
    bool success = false;
    g_user_init_stage = 0;
    g_user_init_result = 0;
    g_user_init_device_registered = false;
    if (!init_register_device() ||
        process_create(0, &process) != K_OK ||
        init_load_path("/init", process, &info, &stack_pointer) != K_OK) {
        goto cleanup;
    }
    process->flags |= PROCESS_FLAG_INIT_CPU_PINNED;
    if (thread_create_user_suspended(process, info.entry, stack_pointer, 0, 0,
                                     &thread) != K_OK) {
        goto cleanup;
    }
    /* /init 是启动协调线程，先固定在引导 CPU，避免首个用户切换跨 CPU 丢失。 */
    /* /init 固定在引导 CPU，避免服务初始化期间跨 CPU 改变地址空间。 */
    if (thread_start(thread) != K_OK) goto cleanup;
    runtime_space = process->vm;
    vm_space_get(runtime_space);
    success = init_wait_for_exit(process, thread, runtime_space);

cleanup:
    if (runtime_space != 0) vm_space_put(runtime_space);
    if (thread != 0) {
        (void)thread_terminate(thread, K_ECANCELED);
        object_put(thread);
    }
    if (process != 0) object_put(process);
    if (g_user_init_device_registered) {
        device_unregister(&g_user_init_device);
        g_user_init_device_registered = false;
    }
    return success;
}

bool user_init_start(void) {
    process_t *process = 0;
    thread_t *thread = 0;
    user_elf_image_info_t info;
    vaddr_t stack_pointer = 0;

    if (g_runtime_init_started) {
        return g_runtime_init_process != 0 && g_runtime_init_thread != 0;
    }
    g_runtime_init_started = true;
    g_user_init_stage = 0;
    g_user_init_result = 0;
    if (!g_user_init_device_registered && !init_register_device()) goto runtime_start_fail;
    if (process_create(0, &process) != K_OK ||
        init_load_path("/init-runtime", process, &info, &stack_pointer) != K_OK) {
        goto runtime_start_fail;
    }
    process->flags |= PROCESS_FLAG_INIT_CPU_PINNED;
    if (thread_create_user_suspended(process, info.entry, stack_pointer, 0, 0,
                                     &thread) != K_OK || thread_start(thread) != K_OK) {
        if (thread != 0) {
            (void)thread_terminate(thread, K_ECANCELED);
            object_put(thread);
        }
        object_put(process);
        g_runtime_init_started = false;
        return false;
    }
    /* 静态引用维持 init 与其地址空间，直到系统关机或用户态自行退出。 */
    g_runtime_init_process = process;
    g_runtime_init_thread = thread;
    return true;

runtime_start_fail:
    if (process != 0) object_put(process);
    g_runtime_init_started = false;
    return false;
}

void user_init_poll(void) {
    process_t *process = g_runtime_init_process;
    thread_t *thread = g_runtime_init_thread;
    if (!g_runtime_init_started || process == 0 || thread == 0 ||
        atomic_load_explicit(&thread->state, memory_order_acquire) != THREAD_DEAD ||
        atomic_load_explicit(&process->state, memory_order_acquire) != PROCESS_DEAD) {
        return;
    }

    /* 先摘下静态引用，再释放对象，避免重启路径观察到半拆除状态。 */
    g_runtime_init_thread = 0;
    g_runtime_init_process = 0;
    g_runtime_init_started = false;
    object_put(thread);
    object_put(process);
    if (g_runtime_init_restart_count >= INIT_RUNTIME_RESTART_LIMIT) return;
    ++g_runtime_init_restart_count;
    (void)user_init_start();
}

uint32_t user_init_failure_stage(void) {
    return g_user_init_stage;
}

int64_t user_init_failure_result(void) {
    return g_user_init_result;
}
