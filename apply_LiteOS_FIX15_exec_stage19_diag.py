#!/usr/bin/env python3
from pathlib import Path

p = Path("kernel/process/elf.c")
if not p.exists():
    raise SystemExit("run this from LiteOS repository root")

s = p.read_text(encoding="utf-8")

def once(old, new, label):
    global s
    n = s.count(old)
    if n != 1:
        raise SystemExit(f"{label}: expected 1 match, found {n}")
    s = s.replace(old, new, 1)

if "#include <kernel/console.h>" not in s:
    once(
        "#include <kernel/elf_loader.h>\n",
        "#include <kernel/elf_loader.h>\n#include <kernel/console.h>\n",
        "console include",
    )

marker = '''static uint64_t g_user_runtime_cpu_current_tid;

enum {
'''
helper = '''static uint64_t g_user_runtime_cpu_current_tid;

/*
 * Exec diagnostics are deliberately failure-only so the successful runtime
 * timing remains unchanged.
 */
static void exec_diag_status(const char *tag, kstatus_t status) {
    liteos_serial_write("LITEOS_DIAG_EXEC_");
    liteos_serial_write(tag);
    liteos_serial_write(" STATUS=");
    liteos_serial_write_u32((uint32_t)status);
    liteos_serial_write("\\r\\n");
}

static void exec_diag_read(kstatus_t status, uint64_t bytes, uint64_t size) {
    liteos_serial_write("LITEOS_DIAG_EXEC_READ_FAIL STATUS=");
    liteos_serial_write_u32((uint32_t)status);
    liteos_serial_write(" BYTES=");
    liteos_serial_write_u32((uint32_t)bytes);
    liteos_serial_write(" SIZE=");
    liteos_serial_write_u32((uint32_t)size);
    liteos_serial_write("\\r\\n");
}

static void exec_diag_prepare(kstatus_t status, uint32_t stage) {
    liteos_serial_write("LITEOS_DIAG_EXEC_PREPARE_FAIL STATUS=");
    liteos_serial_write_u32((uint32_t)status);
    liteos_serial_write(" SUBSTAGE=");
    liteos_serial_write_u32(stage);
    liteos_serial_write("\\r\\n");
}

enum {
'''
if "exec_diag_status(" not in s:
    once(marker, helper, "diagnostic helpers")

old_sig = '''static kstatus_t prepare_elf_space(const void *raw_image, size_t image_size,
                                   vm_space_t **space_out, user_elf_image_info_t *info,
                                   const elf_exec_arguments_t *arguments,
                                   vaddr_t *stack_pointer) {
'''
new_sig = '''static kstatus_t prepare_elf_space(const void *raw_image, size_t image_size,
                                   vm_space_t **space_out, user_elf_image_info_t *info,
                                   const elf_exec_arguments_t *arguments,
                                   vaddr_t *stack_pointer,
                                   uint32_t *diag_stage) {
'''
once(old_sig, new_sig, "prepare signature")

once(
'''    kstatus_t status = validate_elf(image, image_size, ELF_PIE_BIAS,
                                    &main_validation);
    if (status != K_OK) return status;
''',
'''    if (diag_stage != 0) *diag_stage = 1U; /* validate main ELF */
    kstatus_t status = validate_elf(image, image_size, ELF_PIE_BIAS,
                                    &main_validation);
    if (status != K_OK) return status;
''',
"prepare validate main",
)

once(
'''        status = read_vfs_image(interpreter_path, &interpreter_image,
                                &interpreter_size);
        if (status != K_OK) return status;
        status = validate_elf(interpreter_image, interpreter_size, ELF_INTERP_BIAS,
''',
'''        if (diag_stage != 0) *diag_stage = 2U; /* read PT_INTERP */
        status = read_vfs_image(interpreter_path, &interpreter_image,
                                &interpreter_size);
        if (status != K_OK) return status;
        if (diag_stage != 0) *diag_stage = 3U; /* validate PT_INTERP */
        status = validate_elf(interpreter_image, interpreter_size, ELF_INTERP_BIAS,
''',
"prepare interpreter stages",
)

once(
'''    vm_space_t *space = 0;
    status = vm_space_create(&space);
''',
'''    vm_space_t *space = 0;
    if (diag_stage != 0) *diag_stage = 4U; /* create new vm_space */
    status = vm_space_create(&space);
''',
"prepare vm create",
)

once(
'''    status = map_elf_segments(space, image, &main_validation);
    if (status == K_OK && interpreter_image != 0) {
        status = map_elf_segments(space, interpreter_image, &interpreter_validation);
    }
''',
'''    if (diag_stage != 0) *diag_stage = 5U; /* map main PT_LOADs */
    status = map_elf_segments(space, image, &main_validation);
    if (status == K_OK && interpreter_image != 0) {
        if (diag_stage != 0) *diag_stage = 6U; /* map interpreter PT_LOADs */
        status = map_elf_segments(space, interpreter_image, &interpreter_validation);
    }
''',
"prepare map stages",
)

once(
'''    status = build_initial_stack(space, info, arguments, stack_pointer);
    if (status != K_OK) {
''',
'''    if (diag_stage != 0) *diag_stage = 7U; /* build initial user stack */
    status = build_initial_stack(space, info, arguments, stack_pointer);
    if (status != K_OK) {
''',
"prepare stack stage",
)

once(
'''    kfree(interpreter_image);
    *space_out = space;
    return K_OK;
}

kstatus_t process_load_elf_image''',
'''    kfree(interpreter_image);
    *space_out = space;
    if (diag_stage != 0) *diag_stage = 8U;
    return K_OK;
}

kstatus_t process_load_elf_image''',
"prepare success stage",
)

once(
'''    kstatus_t status = prepare_elf_space(image, image_size, &new_space, info,
                                          &arguments, stack_pointer);
''',
'''    kstatus_t status = prepare_elf_space(image, image_size, &new_space, info,
                                          &arguments, stack_pointer, 0);
''',
"process_load caller",
)

old_exec = '''kstatus_t process_exec_from_vfs(
    process_t *process, const char __user *path,
    const char __user *const __user *argv) {
    if (process == 0 || path == 0) return K_EINVAL;
    thread_t *thread = sched_current_thread();
    if (thread == 0 || thread->process != process) return K_EPERM;

    elf_exec_arguments_t arguments;
    kstatus_t status = copy_exec_arguments(argv, &arguments);
    if (status != K_OK) return status;
    file_t *file = 0;
    status = vfs_open(path, VFS_OPEN_READ, 0, &file);
    if (status != K_OK) return status;
    if (file->vnode == 0 || file->vnode->size == 0 ||
        file->vnode->size > ELF_MAX_SEGMENT_SIZE * 4ULL) {
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
    if (status != K_OK || bytes != image_size) {
        kfree(image);
        return status == K_OK ? K_EIO : status;
    }

    vm_space_t *new_space = 0;
    user_elf_image_info_t info;
    vaddr_t stack_pointer = 0;
    status = prepare_elf_space(image, image_size, &new_space, &info,
                               &arguments, &stack_pointer);
    kfree(image);
    if (status != K_OK) return status;

    while (atomic_exchange_explicit(&process->thread_lock.state, 1U,
                                    memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
    if (atomic_load_explicit(&process->state, memory_order_acquire) != PROCESS_RUNNING ||
        process->thread_count != 1U || thread->exec_pending) {
        atomic_store_explicit(&process->thread_lock.state, 0U, memory_order_release);
        vm_space_put(new_space);
        return K_EBUSY;
    }
    vm_space_t *old_space = process->vm;
    process->vm = new_space;
    thread->exec_entry = info.entry;
    thread->exec_stack = stack_pointer;
    thread->exec_pending = true;
    atomic_store_explicit(&process->thread_lock.state, 0U, memory_order_release);
    /* 当前 CPU 仍在旧 CR3 上运行，先切换到新地址空间再释放旧页表。 */
    x86_activate_root_table_pcid(new_space->root_table, new_space->pcid);
    vm_space_put(old_space);
    return K_OK;
}
'''

new_exec = '''kstatus_t process_exec_from_vfs(
    process_t *process, const char __user *path,
    const char __user *const __user *argv) {
    if (process == 0 || path == 0) return K_EINVAL;
    thread_t *thread = sched_current_thread();
    if (thread == 0 || thread->process != process) return K_EPERM;

    elf_exec_arguments_t arguments;
    kstatus_t status = copy_exec_arguments(argv, &arguments);
    if (status != K_OK) {
        exec_diag_status("ARGS_FAIL", status);
        return status;
    }

    file_t *file = 0;
    status = vfs_open(path, VFS_OPEN_READ, 0, &file);
    if (status != K_OK) {
        exec_diag_status("OPEN_FAIL", status);
        return status;
    }
    if (file->vnode == 0 || file->vnode->size == 0 ||
        file->vnode->size > ELF_MAX_SEGMENT_SIZE * 4ULL) {
        vfs_close(file);
        exec_diag_status("SIZE_FAIL", K_EINVAL);
        return K_EINVAL;
    }

    size_t image_size = (size_t)file->vnode->size;
    uint8_t *image = (uint8_t *)kmalloc(image_size, 0);
    if (image == 0) {
        vfs_close(file);
        exec_diag_status("ALLOC_FAIL", K_ENOMEM);
        return K_ENOMEM;
    }

    uint64_t bytes = 0;
    status = vfs_read_kernel(file, image, image_size, &bytes);
    vfs_close(file);
    if (status != K_OK || bytes != image_size) {
        kstatus_t failure = status == K_OK ? K_EIO : status;
        exec_diag_read(failure, bytes, image_size);
        kfree(image);
        return failure;
    }

    vm_space_t *new_space = 0;
    user_elf_image_info_t info;
    vaddr_t stack_pointer = 0;
    uint32_t prepare_stage = 0U;
    status = prepare_elf_space(image, image_size, &new_space, &info,
                               &arguments, &stack_pointer, &prepare_stage);
    kfree(image);
    if (status != K_OK) {
        exec_diag_prepare(status, prepare_stage);
        return status;
    }

    while (atomic_exchange_explicit(&process->thread_lock.state, 1U,
                                    memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
    if (atomic_load_explicit(&process->state, memory_order_acquire) != PROCESS_RUNNING ||
        process->thread_count != 1U || thread->exec_pending) {
        uint32_t thread_count = process->thread_count;
        uint32_t process_state =
            atomic_load_explicit(&process->state, memory_order_relaxed);
        atomic_store_explicit(&process->thread_lock.state, 0U, memory_order_release);
        vm_space_put(new_space);
        liteos_serial_write("LITEOS_DIAG_EXEC_COMMIT_BUSY THREADS=");
        liteos_serial_write_u32(thread_count);
        liteos_serial_write(" STATE=");
        liteos_serial_write_u32(process_state);
        liteos_serial_write("\\r\\n");
        return K_EBUSY;
    }

    vm_space_t *old_space = process->vm;
    process->vm = new_space;
    thread->exec_entry = info.entry;
    thread->exec_stack = stack_pointer;
    thread->exec_pending = true;
    atomic_store_explicit(&process->thread_lock.state, 0U, memory_order_release);
    /* 当前 CPU 仍在旧 CR3 上运行，先切换到新地址空间再释放旧页表。 */
    x86_activate_root_table_pcid(new_space->root_table, new_space->pcid);
    vm_space_put(old_space);
    return K_OK;
}
'''

once(old_exec, new_exec, "process_exec_from_vfs")

p.write_text(s, encoding="utf-8")
print("FIX15-DIAG applied: kernel/process/elf.c")
print("Run: git diff --check && make clean && make")
print("On failure:")
print("  grep -E 'LITEOS_DIAG_EXEC|LITEOS_USER_RUNTIME_FAIL_STAGE' build/qemu-serial.log")
