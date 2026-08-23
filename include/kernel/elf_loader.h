#pragma once

#include <kernel/process.h>

typedef struct user_elf_image_info {
    vaddr_t entry;
    /* 动态 ELF 的解释器入口；静态 ELF 时与 entry 相同。 */
    vaddr_t main_entry;
    /* 动态 ELF 解释器的装载基址，供 AT_BASE 使用。 */
    vaddr_t interpreter_base;
    vaddr_t program_headers;
    uint16_t program_header_size;
    uint16_t program_header_count;
    vaddr_t load_bias;
} user_elf_image_info_t;

kstatus_t process_load_elf_image(process_t *process, const void *image, size_t image_size,
                                 user_elf_image_info_t *info, vaddr_t *stack_pointer);
kstatus_t process_exec_from_vfs(
    process_t *process, const char __user *path,
    const char __user *const __user *argv);
bool user_elf_loader_self_test(void);
bool user_elf_runtime_self_test(void);
uint32_t user_elf_runtime_failure_stage(void);
uint64_t user_elf_runtime_failure_result(void);
bool user_elf_runtime_cow_passed(void);
bool user_elf_runtime_vm_concurrent_passed(void);
bool user_elf_runtime_uaccess_passed(void);
bool user_elf_runtime_wait_race_passed(void);
uint32_t user_elf_runtime_futex_word(void);
uint32_t user_elf_runtime_child_mark(void);
uint32_t user_elf_runtime_thread_count(void);
uint32_t user_elf_runtime_child_state(void);
uint32_t user_elf_runtime_child_cpu(void);
uint32_t user_elf_runtime_child_flags(void);
uint32_t user_elf_runtime_cpu_current_state(void);
uint32_t user_elf_runtime_cpu_runnable(void);
uint64_t user_elf_runtime_cpu_current_tid(void);
uint32_t user_elf_runtime_thread_flags(void);
uint32_t user_elf_runtime_process_flags(void);
uint32_t user_elf_runtime_vm_live(void);
