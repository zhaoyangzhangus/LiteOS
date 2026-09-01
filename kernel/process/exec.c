#include <arch/x86_64/apic.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/paging.h>
#include <arch/x86_64/smp.h>
#include <arch/x86_64/uaccess.h>
#include <kernel/elf_loader.h>
#include <kernel/console.h>
#include <kernel/debug_stage.h>
#include <kernel/deferred.h>
#include <kernel/futex.h>
#include <kernel/wait.h>
#include <kernel/kmem.h>
#include <kernel/sched.h>
#include <kernel/vfs.h>
#include <uapi/mm.h>
#include <uapi/process.h>
#include <uapi/syscall.h>

#include "exec_internal.h"
#include "internal.h"

#ifndef LITEOS_DEBUG_SERIAL
#define LITEOS_DEBUG_SERIAL 0
#endif

#define ELF_EXEC_ARG_MAX      32U
#define ELF_EXEC_ENV_MAX      64U
#define ELF_EXEC_ARG_LENGTH   256U
#define EXEC_PATH_CAPACITY    256U
#define USER_STACK_TOP        0x00007FFFFFF00000ULL
#define USER_STACK_SIZE       (8ULL * 1024ULL * 1024ULL)

/* Failure-only PROCESS_EXEC progress snapshot. */
static atomic_uint g_exec_debug_stage;
static atomic_uint g_exec_debug_status;
static atomic_uint g_exec_debug_thread;

void process_exec_debug_mark(uint32_t stage) {
    thread_t *current = sched_current_thread();
    uint32_t target = atomic_load_explicit(&g_exec_debug_thread,
                                           memory_order_acquire);
    if (current == 0 || target == 0 || (uint32_t)current->tid != target) return;
    atomic_store_explicit(&g_exec_debug_stage, stage, memory_order_release);
}

static void exec_debug_progress(uint32_t stage) {
    process_exec_debug_mark(stage);
}

static void exec_debug_result(kstatus_t status) {
    atomic_store_explicit(&g_exec_debug_status, (uint32_t)status,
                          memory_order_release);
}

uint32_t process_exec_debug_stage(void) {
    return atomic_load_explicit(&g_exec_debug_stage, memory_order_acquire);
}

uint32_t process_exec_debug_status(void) {
    return atomic_load_explicit(&g_exec_debug_status, memory_order_acquire);
}

typedef struct elf_exec_arguments {
    uint32_t count;
    char values[ELF_EXEC_ARG_MAX][ELF_EXEC_ARG_LENGTH];
    uint32_t environment_count;
    char environment[ELF_EXEC_ENV_MAX][ELF_EXEC_ARG_LENGTH];
} elf_exec_arguments_t;

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

static void exec_update_process_name(process_t *process,
                                     const char __user *path) {
    char copy[EXEC_PATH_CAPACITY];
    if (process == 0 || path == 0) return;
    for (size_t index = 0U; index + 1U < sizeof(copy); ++index) {
        if (copy_from_user(&copy[index], path + index, sizeof(copy[index])) != K_OK) {
            return;
        }
        if (copy[index] == '\0') {
            process_set_name(process, copy);
            return;
        }
    }
}

/*
 * Releasing an old address space can unmap every VMA and issue a TLB
 * shootdown for each page.  It must not keep the exec syscall on the old
 * process's execution stack after CR3 has already moved to the new space.
 * The deferred worker runs with a kernel root and an ordinary preemptible
 * context, so the teardown cannot strand the newly loaded user thread.
 */
static void exec_release_old_space_work(void *argument) {
    vm_space_put((vm_space_t *)argument);
}

static void exec_release_old_space(vm_space_t *space) {
    if (space == 0) return;
    /* A full deferred queue is exceptional; preserve ownership with a safe
     * synchronous fallback rather than leaking the address space. */
    if (!deferred_schedule(exec_release_old_space_work, space)) {
        vm_space_put(space);
    }
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
        if (status != K_OK) {
            status = vm_handle_fault(space, &(vm_fault_info_t){
                (vaddr_t)page_address, VM_PROT_WRITE, 0});
            if (status == K_OK) {
                status = x86_translate_page(space->root_table,
                                            (vaddr_t)page_address,
                                            &physical, 0);
            }
        }
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
            /* PT_INTERP 鐨勫瓧绗︿覆鏈韩涓嶄骇鐢熸槧灏勩€?*/
        }
        if (program->type == ELF_PT_DYNAMIC &&
            (program->memory_size < program->file_size ||
             !range_inside(program->offset, program->file_size, image_size) ||
             program->virtual_address > UINT64_MAX - bias ||
             bias + program->virtual_address > UINT64_MAX - program->memory_size)) {
            return K_EINVAL;
        }
        if (program->type == ELF_PT_DYNAMIC) {
            /* 鍔ㄦ€佹鍙敱鐢ㄦ埛鎬佽В閲婂櫒瑙ｆ瀽锛屽唴鏍镐笉鎵ц绗﹀彿閲嶅畾浣嶃€?*/
        }
        if (program->type == ELF_PT_INTERP) {
            /* 鍔ㄦ€侀摼鎺ュ櫒鐢辩敤鎴锋€佽杞斤紱鍐呮牳褰撳墠鍙帴鍙楁棤瑙ｉ噴鍣ㄧ殑闈欐€?ELF銆?*/
            /* 宸茶褰曡В閲婂櫒璺緞锛涙槧灏勫拰鍏ュ彛鍒囨崲鍦ㄨ杞介樁娈靛畬鎴愩€?*/
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
    exec_debug_progress(0x120U);
    uint64_t virtual_start = bias + program->virtual_address;
    uint64_t mapping_start = virtual_start & ~(PAGE_SIZE - 1ULL);
    uint64_t mapping_end;
    if (!align_up_page(virtual_start + program->memory_size, &mapping_end)) return K_EINVAL;
    size_t mapping_size = (size_t)(mapping_end - mapping_start);
    vm_object_t *object = 0;
    kstatus_t status = vm_object_create_anon(mapping_size, &object);
    if (status != K_OK) {
        exec_debug_result(status);
        return status;
    }
    vaddr_t address = (vaddr_t)mapping_start;
    uint32_t protection = vm_protection(program->flags);
    exec_debug_progress(0x121U);
    status = vm_map_object(space, object, &address, 0, mapping_size, protection,
                           VM_MAP_PRIVATE | VM_MAP_FIXED);
    vm_object_put(object);
    if (status != K_OK) {
        exec_debug_result(status);
        return status;
    }
    uint32_t access = fault_access(protection);
    for (uint64_t page = mapping_start; page < mapping_end; page += PAGE_SIZE) {
        exec_debug_progress(0x122U |
                            (uint32_t)((page - mapping_start) >> PAGE_SHIFT));
        status = vm_handle_fault(space, &(vm_fault_info_t){(vaddr_t)page, access, 0});
        if (status != K_OK) {
            exec_debug_result(status);
            return status;
        }
    }
    exec_debug_progress(0x123U);
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

static kstatus_t copy_exec_vector(
    const char __user *const __user *vector, uint32_t maximum,
    char values[][ELF_EXEC_ARG_LENGTH], uint32_t *count) {
    if (count == 0) return K_EINVAL;
    *count = 0U;
    if (vector == 0) return K_OK;
    for (uint32_t index = 0U; index < maximum; ++index) {
        const char __user *value_pointer = 0;
        kstatus_t status = copy_from_user(&value_pointer, vector + index,
                                          sizeof(value_pointer));
        if (status != K_OK) return status;
        if (value_pointer == 0) {
            *count = index;
            return K_OK;
        }
        bool terminated = false;
        for (uint32_t character = 0U; character < ELF_EXEC_ARG_LENGTH; ++character) {
            char value = 0;
            status = copy_from_user(&value, value_pointer + character,
                                    sizeof(value));
            if (status != K_OK) return status;
            values[index][character] = value;
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

static kstatus_t copy_exec_arguments(
    const char __user *const __user *argv,
    const char __user *const __user *envp,
    elf_exec_arguments_t *arguments) {
    kstatus_t status;
    if (arguments == 0) return K_EINVAL;
    status = copy_exec_vector(argv, ELF_EXEC_ARG_MAX, arguments->values,
                              &arguments->count);
    if (status != K_OK) return status;
    return copy_exec_vector(envp, ELF_EXEC_ENV_MAX, arguments->environment,
                            &arguments->environment_count);
}

static kstatus_t build_initial_stack(vm_space_t *space,
                                     const user_elf_image_info_t *info,
                                     const elf_exec_arguments_t *arguments,
                                     const os_exec_fd_map_t *descriptor_map,
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
    if (arguments == 0 || arguments->count > ELF_EXEC_ARG_MAX ||
        arguments->environment_count > ELF_EXEC_ENV_MAX) return K_EINVAL;
    vaddr_t random_address = USER_STACK_TOP - sizeof(random_bytes);
    status = write_address_space(space, random_address, random_bytes, sizeof(random_bytes));
    if (status != K_OK) return status;

    uint64_t argv_values[ELF_EXEC_ARG_MAX + 1U];
    uint64_t environment_values[ELF_EXEC_ENV_MAX + 1U];
    for (uint32_t index = 0U; index < ELF_EXEC_ARG_MAX + 1U; ++index) {
        argv_values[index] = 0U;
    }
    for (uint32_t index = 0U; index < ELF_EXEC_ENV_MAX + 1U; ++index) {
        environment_values[index] = 0U;
    }
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
    for (uint32_t index = arguments->environment_count; index != 0U; --index) {
        const char *environment = arguments->environment[index - 1U];
        size_t length = 0U;
        while (length < ELF_EXEC_ARG_LENGTH && environment[length] != '\0') {
            ++length;
        }
        if (length >= ELF_EXEC_ARG_LENGTH) return K_EINVAL;
        string_pointer -= length + 1U;
        status = write_address_space(space, string_pointer, environment,
                                     length + 1U);
        if (status != K_OK) return status;
        environment_values[index - 1U] = string_pointer;
    }

    vaddr_t descriptor_map_address = 0U;
    if (descriptor_map != 0 && descriptor_map->count != 0U) {
        size_t map_size = sizeof(*descriptor_map);
        string_pointer -= map_size;
        descriptor_map_address = string_pointer;
        status = write_address_space(space, descriptor_map_address,
                                     descriptor_map, map_size);
        if (status != K_OK) return status;
    }

    uint64_t auxiliary[] = {
        AUX_PHDR, info->program_headers,
        AUX_PHENT, info->program_header_size,
        AUX_PHNUM, info->program_header_count,
        AUX_PAGESZ, PAGE_SIZE,
        AUX_BASE, info->interpreter_base,
        AUX_ENTRY, info->main_entry,
        AUX_RANDOM, random_address,
        OS_AUX_LITEOS_FD_MAP, descriptor_map_address,
        AUX_NULL, 0,
    };
    size_t vector_size = sizeof(uint64_t) *
                         (arguments->environment_count + arguments->count + 3U);
    size_t stack_data_size = sizeof(auxiliary) + vector_size;
    vaddr_t stack_top = string_pointer & ~15ULL;
    size_t padding = (size_t)((stack_top - stack_data_size) & 15ULL);
    vaddr_t sp = stack_top - padding;
    /* argc銆乤rgv銆乪nvp 鍜?auxv 缁勬垚鐨勬€婚暱搴﹂殢鍙傛暟涓暟鍙樺寲锛涘伓鏁颁釜鍙傛暟
     * 鏃惰ˉ 8 瀛楄妭锛屼娇鏂扮▼搴忓叆鍙ｄ粛婊¤冻 x86-64 鍒濆鏍?16 瀛楄妭瀵归綈銆?*/
    sp -= sizeof(auxiliary);
    status = write_address_space(space, sp, auxiliary, sizeof(auxiliary));
    if (status != K_OK) return status;
    sp -= sizeof(uint64_t) * (arguments->environment_count + 1U);
    if (write_address_space(space, sp, environment_values,
                            sizeof(uint64_t) *
                            (arguments->environment_count + 1U)) != K_OK) {
        return K_EIO;
    }
    sp -= sizeof(uint64_t) * (arguments->count + 1U);
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

/* 浠?VFS 璇诲彇瑙ｉ噴鍣ㄩ暅鍍忥紱杩斿洖鐨勭紦鍐插尯鐢辫皟鐢ㄨ€呬娇鐢?kfree 閲婃斁銆?*/
static kstatus_t read_vfs_image(const char *path, uint8_t **image_out,
                                size_t *size_out) {
    if (path == 0 || image_out == 0 || size_out == 0) return K_EINVAL;
    *image_out = 0;
    *size_out = 0;
    file_t *file = 0;
    kstatus_t status = vfs_open_kernel(path, VFS_OPEN_READ, 0U, &file);
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
                                   const os_exec_fd_map_t *descriptor_map,
                                   vaddr_t *stack_pointer,
                                   uint32_t *diag_stage) {
    const uint8_t *image = (const uint8_t *)raw_image;
    elf_validation_t main_validation;
    elf_validation_t interpreter_validation;
    uint8_t *interpreter_image = 0;
    size_t interpreter_size = 0;
    exec_debug_progress(0x101U);
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
        exec_debug_progress(0x102U);
        status = read_vfs_image(interpreter_path, &interpreter_image,
                                &interpreter_size);
        if (status != K_OK) return status;
        if (diag_stage != 0) *diag_stage = 3U; /* validate PT_INTERP */
        exec_debug_progress(0x103U);
        status = validate_elf(interpreter_image, interpreter_size, ELF_INTERP_BIAS,
                              &interpreter_validation);
        if (status != K_OK || interpreter_validation.has_interpreter) {
            kfree(interpreter_image);
            return status == K_OK ? K_EINVAL : status;
        }
    }

    vm_space_t *space = 0;
    if (diag_stage != 0) *diag_stage = 4U; /* create new vm_space */
    exec_debug_progress(0x104U);
    status = vm_space_create(&space);
    if (status != K_OK) {
        kfree(interpreter_image);
        return status;
    }
    if (diag_stage != 0) *diag_stage = 5U; /* map main PT_LOADs */
    exec_debug_progress(0x105U);
    status = map_elf_segments(space, image, &main_validation);
    if (status == K_OK && interpreter_image != 0) {
        if (diag_stage != 0) *diag_stage = 6U; /* map interpreter PT_LOADs */
        exec_debug_progress(0x106U);
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
    exec_debug_progress(0x107U);
    status = build_initial_stack(space, info, arguments, descriptor_map,
                                 stack_pointer);
    if (status != K_OK) {
        vm_space_put(space);
        kfree(interpreter_image);
        return status;
    }
    kfree(interpreter_image);
    *space_out = space;
    if (diag_stage != 0) *diag_stage = 8U;
    exec_debug_progress(0x108U);
    return K_OK;
}

kstatus_t process_load_elf_image(process_t *process, const void *image, size_t image_size,
                                 user_elf_image_info_t *info, vaddr_t *stack_pointer) {
    if (process == 0 || info == 0 || stack_pointer == 0) return K_EINVAL;
    elf_exec_arguments_t *arguments =
        (elf_exec_arguments_t *)kzalloc(sizeof(*arguments), 0);
    if (arguments == 0) return K_ENOMEM;
    (void)copy_exec_arguments(0, 0, arguments);
    vm_space_t *new_space = 0;
    kstatus_t status = prepare_elf_space(image, image_size, &new_space, info,
                                          arguments, 0, stack_pointer, 0);
    kfree(arguments);
    if (status != K_OK) return status;
    process_thread_lock(process);
    if (process->thread_count != 0) {
        process_thread_unlock(process);
        vm_space_put(new_space);
        return K_EBUSY;
    }
    vm_space_t *old_space = process->vm;
    process->vm = new_space;
    process_thread_unlock(process);
    vm_space_put(old_space);
    return K_OK;
}

kstatus_t process_exec_from_vfs(
    process_t *process, const char __user *path,
    const char __user *const __user *argv,
    const char __user *const __user *envp,
    const os_exec_fd_map_t *descriptor_map) {
    exec_debug_result(K_OK);
    if (process == 0 || path == 0) {
        exec_debug_result(K_EINVAL);
        return K_EINVAL;
    }
    thread_t *thread = sched_current_thread();
    if (thread == 0 || thread->process != process) {
        exec_debug_result(K_EPERM);
        return K_EPERM;
    }
    atomic_store_explicit(&g_exec_debug_thread, (uint32_t)thread->tid,
                          memory_order_release);
    exec_debug_progress(1U);

    elf_exec_arguments_t *arguments =
        (elf_exec_arguments_t *)kzalloc(sizeof(*arguments), 0);
    if (arguments == 0) {
        exec_debug_result(K_ENOMEM);
        return K_ENOMEM;
    }
    exec_debug_progress(2U);
    kstatus_t status = copy_exec_arguments(argv, envp, arguments);
    if (status != K_OK) {
        kfree(arguments);
        exec_debug_result(status);
        exec_diag_status("ARGS_FAIL", status);
        return status;
    }
    file_t *file = 0;
    exec_debug_progress(3U);
    status = vfs_open(path, VFS_OPEN_READ, 0, &file);
    if (status != K_OK) {
        kfree(arguments);
        exec_debug_result(status);
        exec_diag_status("OPEN_FAIL", status);
        return status;
    }
    if (file->vnode == 0 || file->vnode->size == 0 ||
        file->vnode->size > ELF_MAX_SEGMENT_SIZE * 4ULL) {
        vfs_close(file);
        kfree(arguments);
        exec_debug_result(K_EINVAL);
        exec_diag_status("SIZE_FAIL", K_EINVAL);
        return K_EINVAL;
    }

    size_t image_size = (size_t)file->vnode->size;
    exec_debug_progress(4U);
    uint8_t *image = (uint8_t *)kmalloc(image_size, 0);
    if (image == 0) {
        vfs_close(file);
        kfree(arguments);
        exec_debug_result(K_ENOMEM);
        exec_diag_status("ALLOC_FAIL", K_ENOMEM);
        return K_ENOMEM;
    }

    uint64_t bytes = 0;
    exec_debug_progress(5U);
    status = vfs_read_kernel(file, image, image_size, &bytes);
    vfs_close(file);
    if (status != K_OK || bytes != image_size) {
        kstatus_t failure = status == K_OK ? K_EIO : status;
        exec_debug_result(failure);
        exec_diag_read(failure, bytes, image_size);
        kfree(image);
        kfree(arguments);
        return failure;
    }

    vm_space_t *new_space = 0;
    user_elf_image_info_t info;
    vaddr_t stack_pointer = 0;
    uint32_t prepare_stage = 0U;
    exec_debug_progress(6U);
    status = prepare_elf_space(image, image_size, &new_space, &info,
                               arguments, descriptor_map, &stack_pointer,
                               &prepare_stage);
    kfree(image);
    kfree(arguments);
    if (status != K_OK) {
        exec_debug_result(status);
        exec_diag_prepare(status, prepare_stage);
        return status;
    }

    exec_debug_progress(7U);
    process_thread_lock(process);
    if (atomic_load_explicit(&process->state, memory_order_acquire) != PROCESS_RUNNING ||
        process->thread_count != 1U || thread->exec_pending) {
        uint32_t thread_count = process->thread_count;
        uint32_t process_state =
            atomic_load_explicit(&process->state, memory_order_relaxed);
        process_thread_unlock(process);
        vm_space_put(new_space);
        liteos_serial_write("LITEOS_DIAG_EXEC_COMMIT_BUSY THREADS=");
        liteos_serial_write_u32(thread_count);
        liteos_serial_write(" STATE=");
        liteos_serial_write_u32(process_state);
        liteos_serial_write("\r\n");
        exec_debug_result(K_EBUSY);
        return K_EBUSY;
    }

    vm_space_t *old_space = process->vm;
    process->vm = new_space;
    thread->exec_entry = info.entry;
    thread->exec_stack = stack_pointer;
    thread->exec_pending = true;
    thread->arch.fs_base = 0U;
    thread->signal_frame = 0U;
    thread->signal_depth = 0U;
    spinlock_lock(&process->signal_lock);
    for (uint32_t signal = 1U; signal <= OS_SIGNAL_COUNT; ++signal) {
        if (process->signal_actions[signal].handler != OS_SIG_IGN) {
            process->signal_actions[signal] = (os_signal_action_t){0};
        }
    }
    spinlock_unlock(&process->signal_lock);
    process_thread_unlock(process);
    /* POSIX closes only descriptors marked close-on-exec, and only after the
     * new image has been prepared successfully.  Handle callbacks run outside
     * the table locks so socket/pipe teardown cannot deadlock exec. */
    status = handle_table_close_on_exec(&process->handles);
    if (status != K_OK) {
        /* The image commit is already valid; preserve exec success while
         * leaving a failure-only diagnostic for the exceptional path. */
        exec_diag_status("CLOEXEC_FAIL", status);
    }
    /* 先切到新 CR3，再把旧页表交给可抢占的 deferred worker 回收。 */
    /* Copy the old-image path before switching CR3; the user pointer is not
     * valid in the newly committed address space. */
    exec_update_process_name(process, path);
    exec_debug_progress(8U);
    x86_activate_root_table_pcid(new_space->root_table, new_space->pcid);
    x86_set_user_fs_base(0U);
    x86_fp_state_reset_current(&thread->arch);
    exec_debug_progress(9U);
    exec_release_old_space(old_space);
    exec_debug_progress(10U);
    exec_debug_result(K_OK);
    return K_OK;
}

kstatus_t process_exec(process_t *process, const char __user *path,
                       const char __user *const __user *argv,
                       const char __user *const __user *envp,
                       const os_exec_fd_map_t *descriptor_map) {
    return process_exec_from_vfs(process, path, argv, envp, descriptor_map);
}

static void user_elf_loader_self_test_diag(uint32_t stage,
                                           kstatus_t status) {
    liteos_serial_write("LITEOS_DIAG_USER_ELF_SELFTEST_FAIL STAGE=");
    liteos_serial_write_u32(stage);
    liteos_serial_write(" STATUS=");
    liteos_serial_write_u32((uint32_t)status);
    liteos_serial_write("\r\n");
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
    process_t *dynamic_process = 0;
    user_elf_image_info_t info;
    vaddr_t stack_pointer = 0;
    paddr_t physical;
    kstatus_t status = K_OK;
    uint32_t failure_stage = 0U;
    kstatus_t failure_status = K_EIO;
    bool success = false;
    status = process_create(0, &process);
    if (status != K_OK) {
        failure_stage = 1U;
        failure_status = status;
        goto cleanup;
    }
    status = process_load_elf_image(process, image, sizeof(image), &info,
                                    &stack_pointer);
    if (status != K_OK) {
        failure_stage = 2U;
        failure_status = status;
        goto cleanup;
    }
    if (info.entry != 0x400000ULL || (stack_pointer & 15U) != 0) {
        failure_stage = 3U;
        goto cleanup;
    }
    status = x86_translate_page(process->vm->root_table, info.entry,
                                &physical, 0);
    if (status != K_OK) {
        failure_stage = 4U;
        failure_status = status;
        goto cleanup;
    }
    uint8_t *loaded = (uint8_t *)phys_to_direct(physical);
    if (loaded == 0) {
        failure_stage = 5U;
        goto cleanup;
    }
    for (uint32_t i = 0; i < sizeof(code); ++i) {
        if (loaded[i] != code[i]) {
            failure_stage = 6U;
            goto cleanup;
        }
    }
    status = thread_create_user(process, info.entry, stack_pointer, 0, &thread);
    if (status != K_OK) {
        failure_stage = 7U;
        failure_status = status;
        goto cleanup;
    }
    if (thread->arch.switch_ctx.r12 == 0 || thread->kernel_stack_base == 0) {
        failure_stage = 8U;
        goto cleanup;
    }

    /* 鍐嶉獙璇佷竴娆″甫 PT_INTERP 鐨?ELF锛氳В閲婂櫒鐢?VFS 鍙栧嚭骞跺厛浜庝富绋嬪簭鍏ュ彛鍚姩銆?*/
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
    user_elf_image_info_t dynamic_info;
    vaddr_t dynamic_stack = 0;
    paddr_t dynamic_physical;
    status = process_create(0, &dynamic_process);
    if (status != K_OK) {
        failure_stage = 9U;
        failure_status = status;
        goto cleanup;
    }
    status = process_load_elf_image(dynamic_process, dynamic_image,
                                    sizeof(dynamic_image), &dynamic_info,
                                    &dynamic_stack);
    if (status != K_OK) {
        failure_stage = 10U;
        failure_status = status;
        goto cleanup;
    }
    /* The interpreter's link-time entry belongs to ld-liteos.so.1 and is
     * intentionally not fixed by this kernel self-test.  Validate the bias
     * and the mapped entry range instead of coupling the test to one linker
     * script's entry address. */
    if (dynamic_info.main_entry != 0x400000ULL ||
        dynamic_info.interpreter_base != ELF_INTERP_BIAS ||
        dynamic_info.entry < ELF_INTERP_BIAS ||
        dynamic_info.entry >= ELF_INTERP_BIAS + ELF_MAX_SEGMENT_SIZE) {
        failure_stage = 11U;
        goto cleanup;
    }
    status = x86_translate_page(dynamic_process->vm->root_table,
                                dynamic_info.entry, &dynamic_physical, 0);
    if (status != K_OK) {
        failure_stage = 12U;
        failure_status = status;
        goto cleanup;
    }
    success = true;

cleanup:
    if (thread != 0) {
        (void)thread_terminate(thread, K_ECANCELED);
        object_put(thread);
    }
    if (dynamic_process != 0) {
        (void)process_abort(dynamic_process);
        object_put(dynamic_process);
    }
    if (process != 0) {
        if (thread == 0) (void)process_abort(process);
        object_put(process);
    }
    if (!success && failure_stage != 0U) {
        user_elf_loader_self_test_diag(failure_stage, failure_status);
    }
    return success;
}
