#include <arch/x86_64/apic.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/paging.h>
#include <arch/x86_64/smp.h>
#include <arch/x86_64/uaccess.h>
#include <kernel/elf_loader.h>
#include <kernel/console.h>
#include <kernel/futex.h>
#include <kernel/wait.h>
#include <kernel/kmem.h>
#include <kernel/sched.h>
#include <kernel/vfs.h>
#include <uapi/mm.h>
#include <uapi/process.h>
#include <uapi/syscall.h>

#define ELF_CLASS_64          2U
#define ELF_DATA_LSB          1U
#define ELF_VERSION_CURRENT   1U
#define ELF_TYPE_EXEC         2U
#define ELF_TYPE_DYN          3U
#define ELF_MACHINE_X86_64    62U
#define ELF_PT_LOAD           1U
#define ELF_PT_DYNAMIC        2U
#define ELF_PT_INTERP         3U
#define ELF_PT_PHDR           6U
#define ELF_PF_EXEC           1U
#define ELF_PF_WRITE          2U
#define ELF_PF_READ           4U
#define ELF_MAX_PROGRAMS      128U
#define ELF_MAX_SEGMENT_SIZE  (256ULL * 1024ULL * 1024ULL)
#define ELF_PIE_BIAS          0x0000000040000000ULL
#define ELF_INTERP_BIAS       0x0000000020000000ULL
#define ELF_INTERP_MAX        4096U
#define ELF_EXEC_ARG_MAX      32U
#define ELF_EXEC_ARG_LENGTH   256U
#define USER_STACK_TOP        0x00007FFFFFF00000ULL
#define USER_STACK_SIZE       (8ULL * 1024ULL * 1024ULL)

typedef struct elf_exec_arguments {
    uint32_t count;
    char values[ELF_EXEC_ARG_MAX][ELF_EXEC_ARG_LENGTH];
} elf_exec_arguments_t;

extern const uint8_t liteos_user_runtime_blob_start[];
extern const uint8_t liteos_user_runtime_blob_end[];
extern const uint8_t liteos_user_runtime_child[];
extern const uint8_t liteos_user_runtime_timer_helper[];
extern const uint8_t liteos_user_runtime_vm_worker[];
extern const uint8_t liteos_user_runtime_uaccess_worker[];

static uint32_t g_user_runtime_stage;
static uint64_t g_user_runtime_result;
static uint32_t g_user_runtime_cow_pass;
static uint32_t g_user_runtime_vm_concurrent_pass;
static uint32_t g_user_runtime_uaccess_pass;
static uint32_t g_user_runtime_wait_race_pass;
static uint32_t g_user_runtime_futex_word;
static uint32_t g_user_runtime_child_mark;
static uint32_t g_user_runtime_thread_count;
static uint32_t g_user_runtime_child_state;
static uint32_t g_user_runtime_child_cpu;
static uint32_t g_user_runtime_child_flags;
static uint32_t g_user_runtime_cpu_current_state;
static uint32_t g_user_runtime_cpu_runnable;
static uint64_t g_user_runtime_cpu_current_tid;

/*
 * Exec diagnostics are deliberately failure-only so the successful runtime
 * timing remains unchanged.
 */
static void exec_diag_status(const char *tag, kstatus_t status) {
    liteos_serial_write("LITEOS_DIAG_EXEC_");
    liteos_serial_write(tag);
    liteos_serial_write(" STATUS=");
    liteos_serial_write_u32((uint32_t)status);
    liteos_serial_write("\r\n");
}

static void exec_diag_read(kstatus_t status, uint64_t bytes, uint64_t size) {
    liteos_serial_write("LITEOS_DIAG_EXEC_READ_FAIL STATUS=");
    liteos_serial_write_u32((uint32_t)status);
    liteos_serial_write(" BYTES=");
    liteos_serial_write_u32((uint32_t)bytes);
    liteos_serial_write(" SIZE=");
    liteos_serial_write_u32((uint32_t)size);
    liteos_serial_write("\r\n");
}

static void exec_diag_prepare(kstatus_t status, uint32_t stage) {
    liteos_serial_write("LITEOS_DIAG_EXEC_PREPARE_FAIL STATUS=");
    liteos_serial_write_u32((uint32_t)status);
    liteos_serial_write(" SUBSTAGE=");
    liteos_serial_write_u32(stage);
    liteos_serial_write("\r\n");
}

enum {
    AUX_NULL = 0,
    AUX_PHDR = 3,
    AUX_PHENT = 4,
    AUX_PHNUM = 5,
    AUX_PAGESZ = 6,
    AUX_BASE = 7,
    AUX_ENTRY = 9,
    AUX_RANDOM = 25,
};

typedef struct __attribute__((packed)) {
    uint8_t ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t program_header_offset;
    uint64_t section_header_offset;
    uint32_t flags;
    uint16_t header_size;
    uint16_t program_header_size;
    uint16_t program_header_count;
    uint16_t section_header_size;
    uint16_t section_header_count;
    uint16_t section_name_index;
} elf64_header_t;

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t virtual_address;
    uint64_t physical_address;
    uint64_t file_size;
    uint64_t memory_size;
    uint64_t alignment;
} elf64_program_header_t;

typedef struct elf_validation {
    const elf64_header_t *header;
    uint64_t bias;
    uint64_t entry;
    uint64_t phdr_address;
    uint64_t interp_offset;
    uint64_t interp_size;
    bool has_interpreter;
} elf_validation_t;

static bool range_inside(uint64_t offset, uint64_t length, uint64_t total) {
    return offset <= total && length <= total - offset;
}

static bool power_of_two(uint64_t value) {
    return value != 0 && (value & (value - 1ULL)) == 0;
}

static bool align_up_page(uint64_t value, uint64_t *result) {
    if (value > UINT64_MAX - (PAGE_SIZE - 1ULL)) return false;
    *result = (value + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
    return true;
}

static const elf64_program_header_t *program_header(const uint8_t *image,
                                                     const elf64_header_t *header,
                                                     uint16_t index) {
    return (const elf64_program_header_t *)(image + header->program_header_offset +
           (uint64_t)index * header->program_header_size);
}

static uint32_t vm_protection(uint32_t elf_flags) {
    uint32_t protection = VM_PROT_USER;
    if ((elf_flags & ELF_PF_READ) != 0) protection |= VM_PROT_READ;
    if ((elf_flags & ELF_PF_WRITE) != 0) protection |= VM_PROT_WRITE;
    if ((elf_flags & ELF_PF_EXEC) != 0) protection |= VM_PROT_EXEC;
    return protection;
}

static uint32_t fault_access(uint32_t protection) {
    if ((protection & VM_PROT_WRITE) != 0) return VM_PROT_WRITE;
    if ((protection & VM_PROT_READ) != 0) return VM_PROT_READ;
    return VM_PROT_EXEC;
}

static kstatus_t write_address_space(vm_space_t *space, uint64_t address,
                                     const void *source, size_t size) {
    const uint8_t *input = (const uint8_t *)source;
    while (size != 0) {
        uint64_t page_address = address & ~(PAGE_SIZE - 1ULL);
        uint64_t page_offset = address & (PAGE_SIZE - 1ULL);
        size_t chunk = PAGE_SIZE - (size_t)page_offset;
        if (chunk > size) chunk = size;
        paddr_t physical;
        kstatus_t status = x86_translate_page(space->root_table, (vaddr_t)page_address,
                                              &physical, 0);
        if (status != K_OK) return status;
        uint8_t *destination = (uint8_t *)phys_to_direct(physical);
        if (destination == 0) return K_EIO;
        for (size_t i = 0; i < chunk; ++i) destination[page_offset + i] = input[i];
        address += chunk;
        input += chunk;
        size -= chunk;
    }
    return K_OK;
}

static kstatus_t validate_elf(const uint8_t *image, size_t image_size,
                              uint64_t requested_bias, elf_validation_t *output) {
    if (output == 0) return K_EINVAL;
    if (image == 0 || image_size < sizeof(elf64_header_t)) return K_EINVAL;
    *output = (elf_validation_t){0};
    const elf64_header_t *header = (const elf64_header_t *)image;
    if (header->ident[0] != 0x7FU || header->ident[1] != 'E' ||
        header->ident[2] != 'L' || header->ident[3] != 'F' ||
        header->ident[4] != ELF_CLASS_64 || header->ident[5] != ELF_DATA_LSB ||
        header->ident[6] != ELF_VERSION_CURRENT ||
        (header->type != ELF_TYPE_EXEC && header->type != ELF_TYPE_DYN) ||
        header->machine != ELF_MACHINE_X86_64 || header->version != ELF_VERSION_CURRENT ||
        header->header_size < sizeof(*header) ||
        header->program_header_size < sizeof(elf64_program_header_t) ||
        header->program_header_count == 0 ||
        header->program_header_count > ELF_MAX_PROGRAMS) return K_EINVAL;
    uint64_t table_size = (uint64_t)header->program_header_size *
                          header->program_header_count;
    if (!range_inside(header->program_header_offset, table_size, image_size)) return K_EINVAL;
    uint64_t bias = header->type == ELF_TYPE_DYN ? requested_bias : 0U;
    bool executable_entry = false;
    bool load_found = false;
    uint64_t phdr_address = 0;
    if (header->entry > UINT64_MAX - bias) return K_EINVAL;
    uint64_t entry = bias + header->entry;

    for (uint16_t i = 0; i < header->program_header_count; ++i) {
        const elf64_program_header_t *program = program_header(image, header, i);
        if (program->type == ELF_PT_INTERP) {
            if (output->has_interpreter || program->file_size == 0U ||
                program->file_size > ELF_INTERP_MAX ||
                !range_inside(program->offset, program->file_size, image_size)) {
                return K_EINVAL;
            }
            const uint8_t *path = image + program->offset;
            bool terminated = false;
            for (uint64_t byte = 0; byte < program->file_size; ++byte) {
                if (path[byte] == 0U) {
                    terminated = true;
                    output->interp_size = byte + 1U;
                    break;
                }
            }
            if (!terminated || path[0] != '/') return K_EINVAL;
            output->has_interpreter = true;
            output->interp_offset = program->offset;
            /* PT_INTERP 的字符串本身不产生映射。 */
        }
        if (program->type == ELF_PT_DYNAMIC &&
            (program->memory_size < program->file_size ||
             !range_inside(program->offset, program->file_size, image_size) ||
             program->virtual_address > UINT64_MAX - bias ||
             bias + program->virtual_address > UINT64_MAX - program->memory_size)) {
            return K_EINVAL;
        }
        if (program->type == ELF_PT_DYNAMIC) {
            /* 动态段只由用户态解释器解析，内核不执行符号重定位。 */
        }
        if (program->type == ELF_PT_INTERP) {
            /* 动态链接器由用户态装载；内核当前只接受无解释器的静态 ELF。 */
            /* 已记录解释器路径；映射和入口切换在装载阶段完成。 */
        }
        if (program->type == ELF_PT_PHDR) {
            if (program->virtual_address > UINT64_MAX - bias) return K_EINVAL;
            phdr_address = bias + program->virtual_address;
        }
        if (program->type != ELF_PT_LOAD) continue;
        if (program->memory_size < program->file_size ||
            program->memory_size > ELF_MAX_SEGMENT_SIZE ||
            !range_inside(program->offset, program->file_size, image_size) ||
            program->virtual_address > UINT64_MAX - bias ||
            bias + program->virtual_address > UINT64_MAX - program->memory_size ||
            (program->flags & ~(ELF_PF_READ | ELF_PF_WRITE | ELF_PF_EXEC)) != 0 ||
            (program->flags & (ELF_PF_WRITE | ELF_PF_EXEC)) ==
                (ELF_PF_WRITE | ELF_PF_EXEC)) return K_EINVAL;
        if (program->alignment > 1U &&
            (!power_of_two(program->alignment) ||
             (program->virtual_address & (program->alignment - 1ULL)) !=
             (program->offset & (program->alignment - 1ULL)))) return K_EINVAL;
        if (program->memory_size == 0) continue;
        uint64_t start = bias + program->virtual_address;
        uint64_t end = start + program->memory_size;
        if (start < 0x10000ULL || end > X86_64_USER_TOP + 1ULL || end <= start) return K_EINVAL;
        load_found = true;
        if ((program->flags & ELF_PF_EXEC) != 0 && entry >= start && entry < end) {
            executable_entry = true;
        }
        uint64_t phdr_delta = header->program_header_offset >= program->offset ?
                              header->program_header_offset - program->offset : UINT64_MAX;
        if (phdr_address == 0 && phdr_delta <= program->file_size &&
            table_size <= program->file_size - phdr_delta) {
            phdr_address = start + phdr_delta;
        }
    }
    if (!load_found || !executable_entry) return K_EINVAL;
    output->header = header;
    output->bias = bias;
    output->entry = entry;
    output->phdr_address = phdr_address;
    return K_OK;
}

static kstatus_t map_load_segment(vm_space_t *space, const uint8_t *image,
                                  const elf64_program_header_t *program, uint64_t bias) {
    if (program->type != ELF_PT_LOAD || program->memory_size == 0) return K_OK;
    uint64_t virtual_start = bias + program->virtual_address;
    uint64_t mapping_start = virtual_start & ~(PAGE_SIZE - 1ULL);
    uint64_t mapping_end;
    if (!align_up_page(virtual_start + program->memory_size, &mapping_end)) return K_EINVAL;
    size_t mapping_size = (size_t)(mapping_end - mapping_start);
    vm_object_t *object = 0;
    kstatus_t status = vm_object_create_anon(mapping_size, &object);
    if (status != K_OK) return status;
    vaddr_t address = (vaddr_t)mapping_start;
    uint32_t protection = vm_protection(program->flags);
    status = vm_map_object(space, object, &address, 0, mapping_size, protection,
                           VM_MAP_PRIVATE | VM_MAP_FIXED);
    vm_object_put(object);
    if (status != K_OK) return status;
    uint32_t access = fault_access(protection);
    for (uint64_t page = mapping_start; page < mapping_end; page += PAGE_SIZE) {
        status = vm_handle_fault(space, &(vm_fault_info_t){(vaddr_t)page, access, 0});
        if (status != K_OK) return status;
    }
    return write_address_space(space, virtual_start, image + program->offset,
                               (size_t)program->file_size);
}

static kstatus_t map_elf_segments(vm_space_t *space, const uint8_t *image,
                                  const elf_validation_t *validation) {
    if (space == 0 || image == 0 || validation == 0 || validation->header == 0) {
        return K_EINVAL;
    }
    for (uint16_t i = 0; i < validation->header->program_header_count; ++i) {
        kstatus_t status = map_load_segment(
            space, image,
            program_header(image, validation->header, i), validation->bias);
        if (status != K_OK) return status;
    }
    return K_OK;
}

static kstatus_t copy_exec_arguments(
    const char __user *const __user *argv, elf_exec_arguments_t *arguments) {
    if (arguments == 0) return K_EINVAL;
    arguments->count = 0U;
    if (argv == 0) return K_OK;
    for (uint32_t index = 0U; index < ELF_EXEC_ARG_MAX; ++index) {
        const char __user *argument = 0;
        kstatus_t status = copy_from_user(&argument, argv + index,
                                          sizeof(argument));
        if (status != K_OK) return status;
        if (argument == 0) {
            arguments->count = index;
            return K_OK;
        }
        bool terminated = false;
        for (uint32_t character = 0U; character < ELF_EXEC_ARG_LENGTH; ++character) {
            char value = 0;
            status = copy_from_user(&value, argument + character, sizeof(value));
            if (status != K_OK) return status;
            arguments->values[index][character] = value;
            if (value == '\0') {
                terminated = true;
                break;
            }
            if (character + 1U == ELF_EXEC_ARG_LENGTH) return K_EINVAL;
        }
        if (!terminated) return K_EINVAL;
    }
    return K_EINVAL;
}

static kstatus_t build_initial_stack(vm_space_t *space,
                                     const user_elf_image_info_t *info,
                                     const elf_exec_arguments_t *arguments,
                                     vaddr_t *stack_pointer) {
    vaddr_t guard_address = USER_STACK_TOP - USER_STACK_SIZE - PAGE_SIZE;
    vaddr_t stack_address = USER_STACK_TOP - USER_STACK_SIZE;
    kstatus_t status = vm_map_object(space, 0, &guard_address, 0, PAGE_SIZE, 0,
                                     VM_MAP_PRIVATE | VM_MAP_FIXED | VM_MAP_GUARD);
    if (status != K_OK) return status;
    status = vm_map_object(space, 0, &stack_address, 0, USER_STACK_SIZE,
                           VM_PROT_READ | VM_PROT_WRITE | VM_PROT_USER,
                           VM_MAP_PRIVATE | VM_MAP_FIXED | VM_MAP_STACK);
    if (status != K_OK) return status;
    status = vm_handle_fault(space, &(vm_fault_info_t){USER_STACK_TOP - 1U,
                                                       VM_PROT_WRITE, 0});
    if (status != K_OK) return status;

    uint8_t random_bytes[16];
    uint32_t seed_low;
    uint32_t seed_high;
    __asm__ volatile ("rdtsc" : "=a"(seed_low), "=d"(seed_high));
    uint64_t seed = ((uint64_t)seed_high << 32) | seed_low;
    for (uint32_t i = 0; i < sizeof(random_bytes); ++i) {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        random_bytes[i] = (uint8_t)seed;
    }
    if (arguments == 0 || arguments->count > ELF_EXEC_ARG_MAX) return K_EINVAL;
    vaddr_t random_address = USER_STACK_TOP - sizeof(random_bytes);
    status = write_address_space(space, random_address, random_bytes, sizeof(random_bytes));
    if (status != K_OK) return status;

    uint64_t argv_values[ELF_EXEC_ARG_MAX + 1U] = {0};
    vaddr_t string_pointer = random_address;
    for (uint32_t index = arguments->count; index != 0U; --index) {
        const char *argument = arguments->values[index - 1U];
        size_t length = 0U;
        while (length < ELF_EXEC_ARG_LENGTH && argument[length] != '\0') ++length;
        if (length >= ELF_EXEC_ARG_LENGTH) return K_EINVAL;
        string_pointer -= length + 1U;
        status = write_address_space(space, string_pointer, argument, length + 1U);
        if (status != K_OK) return status;
        argv_values[index - 1U] = string_pointer;
    }

    uint64_t auxiliary[] = {
        AUX_PHDR, info->program_headers,
        AUX_PHENT, info->program_header_size,
        AUX_PHNUM, info->program_header_count,
        AUX_PAGESZ, PAGE_SIZE,
        AUX_BASE, info->interpreter_base,
        AUX_ENTRY, info->main_entry,
        AUX_RANDOM, random_address,
        AUX_NULL, 0,
    };
    vaddr_t sp = string_pointer & ~15ULL;
    /* argc、argv、envp 和 auxv 组成的总长度随参数个数变化；偶数个参数
     * 时补 8 字节，使新程序入口仍满足 x86-64 初始栈 16 字节对齐。 */
    if ((arguments->count & 1U) == 0U) sp -= sizeof(uint64_t);
    sp -= sizeof(auxiliary);
    status = write_address_space(space, sp, auxiliary, sizeof(auxiliary));
    if (status != K_OK) return status;
    uint64_t zero = 0;
    sp -= sizeof(uint64_t); /* envp[0] */
    if (write_address_space(space, sp, &zero, sizeof(zero)) != K_OK) return K_EIO;
    sp -= sizeof(uint64_t) * (arguments->count + 1U); /* argv[] and terminator */
    if (write_address_space(space, sp, argv_values,
                            sizeof(uint64_t) * (arguments->count + 1U)) != K_OK) {
        return K_EIO;
    }
    uint64_t argc = arguments->count;
    sp -= sizeof(uint64_t); /* argc */
    if (write_address_space(space, sp, &argc, sizeof(argc)) != K_OK) return K_EIO;
    if ((sp & 15U) != 0) return K_EIO;
    *stack_pointer = sp;
    return K_OK;
}

/* 从 VFS 读取解释器镜像；返回的缓冲区由调用者使用 kfree 释放。 */
static kstatus_t read_vfs_image(const char *path, uint8_t **image_out,
                                size_t *size_out) {
    if (path == 0 || image_out == 0 || size_out == 0) return K_EINVAL;
    *image_out = 0;
    *size_out = 0;
    file_t *file = 0;
    kstatus_t status = vfs_open_kernel(path, VFS_OPEN_READ, &file);
    if (status != K_OK) return status;
    if (file->vnode == 0 || file->vnode->size == 0U ||
        file->vnode->size > (uint64_t)ELF_MAX_SEGMENT_SIZE * 4ULL ||
        file->vnode->size > (uint64_t)SIZE_MAX) {
        vfs_close(file);
        return K_EINVAL;
    }
    size_t size = (size_t)file->vnode->size;
    uint8_t *image = (uint8_t *)kmalloc(size, 0);
    if (image == 0) {
        vfs_close(file);
        return K_ENOMEM;
    }
    uint64_t bytes = 0;
    status = vfs_read_kernel(file, image, size, &bytes);
    vfs_close(file);
    if (status != K_OK || bytes != size) {
        kfree(image);
        return status == K_OK ? K_EIO : status;
    }
    *image_out = image;
    *size_out = size;
    return K_OK;
}

static kstatus_t prepare_elf_space(const void *raw_image, size_t image_size,
                                   vm_space_t **space_out, user_elf_image_info_t *info,
                                   const elf_exec_arguments_t *arguments,
                                   vaddr_t *stack_pointer,
                                   uint32_t *diag_stage) {
    const uint8_t *image = (const uint8_t *)raw_image;
    elf_validation_t main_validation;
    elf_validation_t interpreter_validation;
    uint8_t *interpreter_image = 0;
    size_t interpreter_size = 0;
    if (diag_stage != 0) *diag_stage = 1U; /* validate main ELF */
    kstatus_t status = validate_elf(image, image_size, ELF_PIE_BIAS,
                                    &main_validation);
    if (status != K_OK) return status;

    if (main_validation.has_interpreter) {
        char interpreter_path[ELF_INTERP_MAX];
        if (main_validation.interp_size == 0U ||
            main_validation.interp_size > sizeof(interpreter_path)) return K_EINVAL;
        for (uint64_t i = 0; i < main_validation.interp_size; ++i) {
            interpreter_path[i] = (char)image[main_validation.interp_offset + i];
        }
        if (diag_stage != 0) *diag_stage = 2U; /* read PT_INTERP */
        status = read_vfs_image(interpreter_path, &interpreter_image,
                                &interpreter_size);
        if (status != K_OK) return status;
        if (diag_stage != 0) *diag_stage = 3U; /* validate PT_INTERP */
        status = validate_elf(interpreter_image, interpreter_size, ELF_INTERP_BIAS,
                              &interpreter_validation);
        if (status != K_OK || interpreter_validation.has_interpreter) {
            kfree(interpreter_image);
            return status == K_OK ? K_EINVAL : status;
        }
    }

    vm_space_t *space = 0;
    if (diag_stage != 0) *diag_stage = 4U; /* create new vm_space */
    status = vm_space_create(&space);
    if (status != K_OK) {
        kfree(interpreter_image);
        return status;
    }
    if (diag_stage != 0) *diag_stage = 5U; /* map main PT_LOADs */
    status = map_elf_segments(space, image, &main_validation);
    if (status == K_OK && interpreter_image != 0) {
        if (diag_stage != 0) *diag_stage = 6U; /* map interpreter PT_LOADs */
        status = map_elf_segments(space, interpreter_image, &interpreter_validation);
    }
    if (status != K_OK) {
        vm_space_put(space);
        kfree(interpreter_image);
        return status;
    }
    info->main_entry = (vaddr_t)main_validation.entry;
    info->entry = interpreter_image != 0 ?
                  (vaddr_t)interpreter_validation.entry : info->main_entry;
    info->interpreter_base = interpreter_image != 0 ?
                             (vaddr_t)interpreter_validation.bias : 0U;
    info->program_headers = (vaddr_t)main_validation.phdr_address;
    info->program_header_size = main_validation.header->program_header_size;
    info->program_header_count = main_validation.header->program_header_count;
    info->load_bias = (vaddr_t)main_validation.bias;
    if (diag_stage != 0) *diag_stage = 7U; /* build initial user stack */
    status = build_initial_stack(space, info, arguments, stack_pointer);
    if (status != K_OK) {
        vm_space_put(space);
        kfree(interpreter_image);
        return status;
    }
    kfree(interpreter_image);
    *space_out = space;
    if (diag_stage != 0) *diag_stage = 8U;
    return K_OK;
}

kstatus_t process_load_elf_image(process_t *process, const void *image, size_t image_size,
                                 user_elf_image_info_t *info, vaddr_t *stack_pointer) {
    if (process == 0 || info == 0 || stack_pointer == 0) return K_EINVAL;
    elf_exec_arguments_t arguments;
    (void)copy_exec_arguments(0, &arguments);
    vm_space_t *new_space = 0;
    kstatus_t status = prepare_elf_space(image, image_size, &new_space, info,
                                          &arguments, stack_pointer, 0);
    if (status != K_OK) return status;
    while (atomic_exchange_explicit(&process->thread_lock.state, 1U,
                                     memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
    if (process->thread_count != 0) {
        atomic_store_explicit(&process->thread_lock.state, 0U, memory_order_release);
        vm_space_put(new_space);
        return K_EBUSY;
    }
    vm_space_t *old_space = process->vm;
    process->vm = new_space;
    atomic_store_explicit(&process->thread_lock.state, 0U, memory_order_release);
    vm_space_put(old_space);
    return K_OK;
}

kstatus_t process_exec_from_vfs(
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
        liteos_serial_write("\r\n");
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

bool user_elf_loader_self_test(void) {
    uint8_t image[0x1008U];
    static uint8_t dynamic_image[0x3000U];
    for (size_t i = 0; i < sizeof(image); ++i) image[i] = 0;
    elf64_header_t *header = (elf64_header_t *)image;
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
    header->entry = 0x400000ULL;
    header->program_header_offset = sizeof(*header);
    header->header_size = sizeof(*header);
    header->program_header_size = sizeof(elf64_program_header_t);
    header->program_header_count = 1U;
    elf64_program_header_t *program = (elf64_program_header_t *)(image + sizeof(*header));
    program->type = ELF_PT_LOAD;
    program->flags = ELF_PF_READ | ELF_PF_EXEC;
    program->offset = 0x1000U;
    program->virtual_address = 0x400000ULL;
    program->file_size = 7U;
    program->memory_size = PAGE_SIZE;
    program->alignment = PAGE_SIZE;
    const uint8_t code[7] = {0x31, 0xFF, 0x31, 0xC0, 0x0F, 0x05, 0xF4};
    for (uint32_t i = 0; i < sizeof(code); ++i) image[0x1000U + i] = code[i];

    process_t *process = 0;
    thread_t *thread = 0;
    user_elf_image_info_t info;
    vaddr_t stack_pointer = 0;
    paddr_t physical;
    bool success = false;
    if (process_create(0, &process) != K_OK ||
        process_load_elf_image(process, image, sizeof(image), &info, &stack_pointer) != K_OK ||
        info.entry != 0x400000ULL || (stack_pointer & 15U) != 0 ||
        x86_translate_page(process->vm->root_table, info.entry, &physical, 0) != K_OK) goto cleanup;
    uint8_t *loaded = (uint8_t *)phys_to_direct(physical);
    if (loaded == 0) goto cleanup;
    for (uint32_t i = 0; i < sizeof(code); ++i) {
        if (loaded[i] != code[i]) goto cleanup;
    }
    if (thread_create_user(process, info.entry, stack_pointer, 0, &thread) != K_OK) goto cleanup;
    success = thread->arch.switch_ctx.r12 != 0 && thread->kernel_stack_base != 0;
    if (!success) goto cleanup;
    success = false;

    /* 再验证一次带 PT_INTERP 的 ELF：解释器由 VFS 取出并先于主程序入口启动。 */
    for (size_t i = 0; i < sizeof(dynamic_image); ++i) dynamic_image[i] = 0;
    elf64_header_t *dynamic_header = (elf64_header_t *)dynamic_image;
    *dynamic_header = *header;
    dynamic_header->program_header_count = 2U;
    elf64_program_header_t *dynamic_programs =
        (elf64_program_header_t *)(dynamic_image + sizeof(*dynamic_header));
    dynamic_programs[0] = *program;
    dynamic_programs[1].type = ELF_PT_INTERP;
    dynamic_programs[1].offset = 0x2000U;
    dynamic_programs[1].file_size = sizeof("/lib/ld-liteos.so.1");
    const char interpreter_path[] = "/lib/ld-liteos.so.1";
    for (size_t i = 0; i < sizeof(interpreter_path); ++i) {
        dynamic_image[0x2000U + i] = (uint8_t)interpreter_path[i];
    }
    process_t *dynamic_process = 0;
    user_elf_image_info_t dynamic_info;
    vaddr_t dynamic_stack = 0;
    paddr_t dynamic_physical;
    if (process_create(0, &dynamic_process) != K_OK ||
        process_load_elf_image(dynamic_process, dynamic_image,
                                sizeof(dynamic_image), &dynamic_info,
                                &dynamic_stack) != K_OK ||
        dynamic_info.main_entry != 0x400000ULL ||
        dynamic_info.entry != ELF_INTERP_BIAS + 0x400000ULL ||
        dynamic_info.interpreter_base != ELF_INTERP_BIAS ||
        x86_translate_page(dynamic_process->vm->root_table, dynamic_info.entry,
                           &dynamic_physical, 0) != K_OK) {
        if (dynamic_process != 0) object_put(dynamic_process);
        goto cleanup;
    }
    object_put(dynamic_process);
    success = true;

cleanup:
    if (thread != 0) {
        (void)thread_terminate(thread, K_ECANCELED);
        object_put(thread);
    }
    if (process != 0) object_put(process);
    return success;
}

bool user_elf_runtime_self_test(void) {
    uint8_t image[0x3200U];
    for (size_t i = 0; i < sizeof(image); ++i) image[i] = 0;
    elf64_header_t *header = (elf64_header_t *)image;
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
    header->entry = 0x400000ULL;
    header->program_header_offset = sizeof(*header);
    header->header_size = sizeof(*header);
    header->program_header_size = sizeof(elf64_program_header_t);
    header->program_header_count = 2U;
    elf64_program_header_t *programs =
        (elf64_program_header_t *)(image + sizeof(*header));
    elf64_program_header_t *code_program = &programs[0];
    code_program->type = ELF_PT_LOAD;
    code_program->flags = ELF_PF_READ | ELF_PF_EXEC;
    code_program->offset = 0x1000U;
    code_program->virtual_address = 0x400000ULL;
    code_program->memory_size = 2U * PAGE_SIZE;
    code_program->alignment = PAGE_SIZE;
    size_t code_size = (size_t)(liteos_user_runtime_blob_end -
                                liteos_user_runtime_blob_start);
    if (code_size == 0 || code_size > 2U * PAGE_SIZE) return false;
    code_program->file_size = code_size;
    for (size_t i = 0; i < code_size; ++i) {
        image[0x1000U + i] = liteos_user_runtime_blob_start[i];
    }

    elf64_program_header_t *data_program = &programs[1];
    data_program->type = ELF_PT_LOAD;
    data_program->flags = ELF_PF_READ | ELF_PF_WRITE;
    data_program->offset = 0x3000U;
    data_program->virtual_address = 0x402000ULL;
    data_program->file_size = 0x1E0U;
    data_program->memory_size = PAGE_SIZE;
    data_program->alignment = PAGE_SIZE;
    os_vm_map_args_t *arguments = (os_vm_map_args_t *)(image + 0x3000U);
    arguments->hdr.size = sizeof(*arguments);
    arguments->hdr.version = OS_SYSCALL_ABI_VERSION;
    arguments->length = PAGE_SIZE;
    arguments->prot = OS_VM_READ | OS_VM_WRITE;
    arguments->flags = OS_VM_PRIVATE;

    os_vm_map_args_t *stack_arguments =
        (os_vm_map_args_t *)(image + 0x3000U + 0x30U);
    stack_arguments->hdr.size = sizeof(*stack_arguments);
    stack_arguments->hdr.version = OS_SYSCALL_ABI_VERSION;
    stack_arguments->length = PAGE_SIZE;
    stack_arguments->prot = OS_VM_READ | OS_VM_WRITE;
    stack_arguments->flags = OS_VM_PRIVATE | OS_VM_STACK;

    os_thread_create_t *thread_arguments =
        (os_thread_create_t *)(image + 0x3000U + 0x60U);
    thread_arguments->hdr.size = sizeof(*thread_arguments);
    thread_arguments->hdr.version = OS_SYSCALL_ABI_VERSION;
    thread_arguments->entry = 0x400000ULL +
        (uint64_t)(liteos_user_runtime_child - liteos_user_runtime_blob_start);
    thread_arguments->argument = 0x1234U;

    os_thread_create_t *timer_arguments =
        (os_thread_create_t *)(image + 0x3000U + 0xC0U);
    timer_arguments->hdr.size = sizeof(*timer_arguments);
    timer_arguments->hdr.version = OS_SYSCALL_ABI_VERSION;
    timer_arguments->entry = 0x400000ULL +
        (uint64_t)(liteos_user_runtime_timer_helper - liteos_user_runtime_blob_start);
    timer_arguments->argument = 0x5678U;
    *(uint64_t *)(void *)(image + 0x3000U + 0xF0U) = 0x400000ULL +
        (uint64_t)(liteos_user_runtime_vm_worker - liteos_user_runtime_blob_start);
    static const char boot_path[] = "/sbin/gshell";
    for (size_t i = 0; i < sizeof(boot_path); ++i) {
        image[0x3000U + 0x100U + i] = (uint8_t)boot_path[i];
    }
    static const char fat32_path[] = "/sbin/notepad";
    for (size_t i = 0; i < sizeof(fat32_path); ++i) {
        image[0x3000U + 0x180U + i] = (uint8_t)fat32_path[i];
    }
    static const char exec_path[] = "/boot/exec-exit.elf";
    for (size_t i = 0; i < sizeof(exec_path); ++i) {
        image[0x3000U + 0x110U + i] = (uint8_t)exec_path[i];
    }

    process_t *process = 0;
    thread_t *thread = 0;
    vm_space_t *runtime_space = 0;
    user_elf_image_info_t info;
    vaddr_t stack_pointer = 0;
    paddr_t kernel_root = x86_current_root_table();
    uint64_t timer_ticks_before = liteos_lapic_tick_count();
    paddr_t marker_physical;
    bool success = false;
    g_user_runtime_stage = 0;
    g_user_runtime_result = 0;
    g_user_runtime_cow_pass = 0;
    g_user_runtime_vm_concurrent_pass = 0;
    g_user_runtime_uaccess_pass = 0;
    g_user_runtime_wait_race_pass = 0;
    g_user_runtime_futex_word = 0;
    g_user_runtime_child_mark = 0;
    g_user_runtime_thread_count = 0;
    g_user_runtime_child_state = UINT32_MAX;
    g_user_runtime_child_cpu = UINT32_MAX;
    g_user_runtime_child_flags = 0;
    g_user_runtime_cpu_current_state = UINT32_MAX;
    g_user_runtime_cpu_runnable = 0;
    g_user_runtime_cpu_current_tid = 0;
    if (process_create(0, &process) != K_OK ||
        process_load_elf_image(process, image, sizeof(image), &info, &stack_pointer) != K_OK ||
        thread_create_user_suspended(process, info.entry, stack_pointer, 0, 0,
                                     &thread) != K_OK) {
        goto cleanup;
    }
    if (x86_smp_discovered_count() > 1U) {
        cpumask_t remote_mask = {0};
        uint32_t current_cpu = x86_current_cpu_index();
        uint32_t remote_cpu = UINT32_MAX;
        for (uint32_t cpu = 0; cpu < x86_smp_discovered_count(); ++cpu) {
            if (cpu != current_cpu && x86_smp_cpu_online(cpu)) {
                remote_cpu = cpu;
                break;
            }
        }
        if (remote_cpu == UINT32_MAX) goto cleanup;
        remote_mask.bits[remote_cpu >> 6] = 1ULL << (remote_cpu & 63U);
        if (sched_set_affinity(thread, &remote_mask) != K_OK) goto cleanup;
    }
    if (thread_start(thread) != K_OK) goto cleanup;
    runtime_space = process->vm;
    vm_space_get(runtime_space);

    /*
     * 一次 schedule() 只保证发生一次调度决策，不能保证用户进程已经退出。
     * 当所有用户线程短暂阻塞时，idle 会恢复这里；此时若立即 cleanup，就会把
     * 仍可能被定时器唤醒的进程释放掉。持续驱动调度，直到主线程和进程均发布
     * DEAD，或达到自测的硬截止时间。
     */
    uint64_t runtime_deadline = x86_read_tsc();
    uint64_t runtime_budget = x86_timeout_ns_to_tsc(15000000000ULL);
    runtime_deadline = runtime_budget > UINT64_MAX - runtime_deadline ?
                       UINT64_MAX : runtime_deadline + runtime_budget;
    for (;;) {
        /* 引导 idle 尚无独立上下文，显式恢复 IF 以接收定时器和重调度 IPI。 */
        __asm__ volatile ("sti" : : : "memory");
        schedule();
        sched_finish_switch();
        if (atomic_load_explicit(&thread->state, memory_order_acquire) == THREAD_DEAD &&
            atomic_load_explicit(&process->state, memory_order_acquire) == PROCESS_DEAD) {
            break;
        }
        if ((int64_t)(x86_read_tsc() - runtime_deadline) >= 0) break;
        __asm__ volatile ("pause");
    }
    /*
     * USER RUNTIME VM REAPER DRAIN
     *
     * PROCESS_DEAD only means process exit has been published.
     * The execution reference may still keep process->vm alive
     * until sched_finish_switch() completes on the execution CPU.
     *
     * This is the same teardown window handled by user_init.
     */
    __asm__ volatile ("sti" : : : "memory");

    for (uint32_t attempt = 0;
         attempt < 4U && process->vm != 0;
         ++attempt)
    {
        sched_finish_switch();

        if(process->vm == 0)
        {
            break;
        }

        schedule();
    }

    /*
     * Execution-ref teardown belongs to the CPU that actually switched away
     * from the dead thread.  Do not clear a remote thread's execution ref from
     * this bootstrap/self-test CPU: the remote CPU may still be using that
     * thread's kernel stack.
     *
     * thread_release_execution_ref() now publishes EXECUTION_REF=0 only after
     * process runtime resources (including process->vm) are released, so an
     * acquire load of the flag is also the teardown-completion barrier.
     */
    uint64_t reap_deadline = x86_read_tsc();
    uint64_t reap_budget = x86_timeout_ns_to_tsc(1000000000ULL);
    reap_deadline = reap_budget > UINT64_MAX - reap_deadline ?
                    UINT64_MAX : reap_deadline + reap_budget;

    while ((__atomic_load_n(&thread->flags, __ATOMIC_ACQUIRE) &
            THREAD_FLAG_EXECUTION_REF) != 0U) {
        __asm__ volatile ("sti" : : : "memory");

        /* Handle a local pending reaper immediately. */
        sched_finish_switch();

        if ((__atomic_load_n(&thread->flags, __ATOMIC_ACQUIRE) &
             THREAD_FLAG_EXECUTION_REF) == 0U) {
            break;
        }

        /*
         * A remote CPU may be sitting in its idle path after this thread
         * exited.  Nudge it so its post-switch reaper runs promptly.
         */
        uint32_t reap_cpu = thread->current_cpu;
        uint32_t current_cpu = x86_current_cpu_index();
        if (reap_cpu < MAX_CPUS && reap_cpu != current_cpu &&
            x86_smp_cpu_online(reap_cpu)) {
            (void)x86_smp_request_reschedule(reap_cpu);
        }

        if ((int64_t)(x86_read_tsc() - reap_deadline) >= 0) break;

        schedule();
        __asm__ volatile ("pause");
    }

    /* Acquire above guarantees process->vm teardown is visible after ref clear. */

    thread_t *current = sched_current_thread();
    g_user_runtime_thread_count = process->thread_count;
    for (list_head_t *node = process->threads.next;
         node != &process->threads; node = node->next) {
        thread_t *candidate = (thread_t *)((uint8_t *)node -
            __builtin_offsetof(thread_t, process_node));
        if (candidate != thread) {
            g_user_runtime_child_state = atomic_load_explicit(&candidate->state,
                                                               memory_order_acquire);
            g_user_runtime_child_cpu = candidate->current_cpu;
            g_user_runtime_child_flags = candidate->sched.flags;
            break;
        }
    }
    /*
     * Runtime wait diagnostic:
     * 如果辅助线程已经退出，复用 CHILD_* 输出 main thread 本身。
     *
     * CHILD_STATE:
     *   0 NEW
     *   1 READY
     *   2 RUNNING
     *   3 BLOCKED
     *   4 STOPPED
     *   5 DEAD
     *
     * CHILD_FLAGS:
     *   bit0      = SCHED_ENTITY_ENQUEUED
     *   bit16     = blocked_waiter != NULL
     *   bits24-31 = waiter_state
     *               0 WAITING
     *               1 WOKEN
     *               2 TIMED_OUT
     *               3 CANCELLED
     */
    if (g_user_runtime_child_state == UINT32_MAX && thread != 0) {
        g_user_runtime_child_state =
            atomic_load_explicit(&thread->state, memory_order_acquire);
        g_user_runtime_child_cpu = thread->current_cpu;

        uint32_t diag_flags = thread->sched.flags;
        waiter_t *diag_waiter = thread->blocked_waiter;
        if (diag_waiter != 0) {
            diag_flags |= (1U << 16);
            diag_flags |=
                (atomic_load_explicit(&diag_waiter->state,
                                      memory_order_acquire) & 0xFFU) << 24;
        }
        g_user_runtime_child_flags = diag_flags;
    }

    if (g_user_runtime_child_cpu != UINT32_MAX) {
        (void)sched_debug_cpu(g_user_runtime_child_cpu,
                              &g_user_runtime_cpu_current_state,
                              &g_user_runtime_cpu_current_tid,
                              &g_user_runtime_cpu_runnable);
    }
    uint8_t marker = 0;
    if (x86_translate_page(runtime_space->root_table, 0x402000ULL,
                           &marker_physical, 0) == K_OK) {
        uint8_t *page = (uint8_t *)phys_to_direct(marker_physical);
        if (page != 0) {
            g_user_runtime_futex_word = *(const uint32_t *)(const void *)(page + 0xA0U);
            g_user_runtime_child_mark = *(const uint32_t *)(const void *)(page + 0xA4U);
            marker = page[0xA4U];
            g_user_runtime_stage = page[0xA8U];
            g_user_runtime_result = *(const uint64_t *)(const void *)(page + 0xB0U);
            g_user_runtime_cow_pass = page[0xBCU];
            /* VM 并发验收标记位于用户数据页的 0x1F0，避开定时器参数区。 */
            g_user_runtime_vm_concurrent_pass = page[0x1F0U];
            g_user_runtime_uaccess_pass = page[0x1F2U];
            g_user_runtime_wait_race_pass = page[0x1F3U];
        }
    }
    bool thread_dead = atomic_load_explicit(&thread->state, memory_order_acquire) == THREAD_DEAD;
    bool process_dead = atomic_load_explicit(&process->state, memory_order_acquire) == PROCESS_DEAD;
    bool tick_seen = liteos_lapic_tick_count() > timer_ticks_before;
    bool kernel_thread_current = current != 0 && current->process == 0;
    bool kernel_root_current = x86_current_root_table().value == kernel_root.value;
    success = thread_dead && thread->exit_code == 0 && process->thread_count == 0 &&
              process_dead && process->exit_code == 0 && process->vm == 0 &&
              marker == 0xA5U && tick_seen && kernel_thread_current &&
              kernel_root_current;
    if (!success && g_user_runtime_result == 0U) {
        uint64_t failure = 0U;
        if (!thread_dead) failure |= 1U;
        if (thread->exit_code != 0) failure |= 2U;
        if (process->thread_count != 0) failure |= 4U;
        if (!process_dead) failure |= 8U;
        if (process->exit_code != 0) failure |= 16U;
        if (process->vm != 0) failure |= 32U;
        if (marker != 0xA5U) failure |= 64U;
        if (!tick_seen) failure |= 128U;
        if (!kernel_thread_current) failure |= 256U;
        if (!kernel_root_current) failure |= 512U;
        g_user_runtime_result = failure;
    }

cleanup:
    if (runtime_space != 0) vm_space_put(runtime_space);
    if (thread != 0) {
        (void)thread_terminate(thread, K_ECANCELED);
        object_put(thread);
    }
    if (process != 0) object_put(process);
    return success;
}

uint32_t user_elf_runtime_failure_stage(void) {
    return g_user_runtime_stage;
}

uint64_t user_elf_runtime_failure_result(void) {
    return g_user_runtime_result;
}

bool user_elf_runtime_cow_passed(void) {
    return g_user_runtime_cow_pass == 0xC0U;
}

bool user_elf_runtime_vm_concurrent_passed(void) {
    return g_user_runtime_vm_concurrent_pass == 0xD2U;
}

bool user_elf_runtime_uaccess_passed(void) {
    return g_user_runtime_uaccess_pass == 0xA7U;
}

bool user_elf_runtime_wait_race_passed(void) {
    return g_user_runtime_wait_race_pass == 0xB7U;
}

uint32_t user_elf_runtime_futex_word(void) {
    return g_user_runtime_futex_word;
}

uint32_t user_elf_runtime_child_mark(void) {
    return g_user_runtime_child_mark;
}

uint32_t user_elf_runtime_thread_count(void) {
    return g_user_runtime_thread_count;
}

uint32_t user_elf_runtime_child_state(void) {
    return g_user_runtime_child_state;
}

uint32_t user_elf_runtime_child_cpu(void) {
    return g_user_runtime_child_cpu;
}

uint32_t user_elf_runtime_child_flags(void) {
    return g_user_runtime_child_flags;
}

uint32_t user_elf_runtime_cpu_current_state(void) {
    return g_user_runtime_cpu_current_state;
}

uint32_t user_elf_runtime_cpu_runnable(void) {
    return g_user_runtime_cpu_runnable;
}

uint64_t user_elf_runtime_cpu_current_tid(void) {
    return g_user_runtime_cpu_current_tid;
}
