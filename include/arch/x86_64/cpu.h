#ifndef LITEOS_ARCH_X86_64_CPU_H
#define LITEOS_ARCH_X86_64_CPU_H

#include <kernel/base.h>
#include <kernel/cpumask.h>
#include "uefi.h"

typedef struct x86_cpu_features {
    bool nx;
    bool smep;
    bool smap;
    bool pcid;
    bool invpcid;
    bool xsave;
    bool x2apic;
    bool invariant_tsc;
    bool tsc_deadline;
    uint8_t phys_bits;
    uint8_t virt_bits;
    uint64_t tsc_hz;
} x86_cpu_features_t;

/*
 * GS 指向的每 CPU 数据。前五个字段同时是 SYSCALL 汇编入口使用的固定 ABI，
 * 因此只能在结构尾部追加字段，不能调整既有字段的顺序。
 */
typedef struct x86_cpu_local {
    uint64_t UserStack;
    uint64_t KernelStack;
    uint64_t KernelResumeStack;
    uint64_t ReturnToKernel;
    uint64_t UserExitSeen;
    uint64_t Self;
    uint32_t CpuIndex;
    uint32_t ApicId;
    uint32_t Flags;
    uint32_t Reserved;
    uint64_t UserEntries;
} x86_cpu_local_t;

#define X86_CPU_LOCAL_ONLINE (1U << 0)

#define X86_PROTECTION_WRITE_PROTECT (1U << 0)
#define X86_PROTECTION_NX            (1U << 1)
#define X86_PROTECTION_SMEP          (1U << 2)
#define X86_PROTECTION_SMAP          (1U << 3)

extern x86_cpu_features_t x86_boot_cpu_features;
void x86_cpu_detect_features(void);
void x86_cpu_enable_protection(void);
uint32_t x86_cpu_protection_status(void);
bool x86_cpu_hardening_self_test(void);
bool x86_cpu_bind_bootstrap(uint32_t cpu_index, uint32_t apic_id);
bool x86_cpu_prepare_secondary(uint32_t cpu_index, uint32_t apic_id,
                               vaddr_t kernel_stack_top);
bool x86_cpu_init_secondary(uint32_t cpu_index, uint32_t apic_id);
x86_cpu_local_t *x86_cpu_local_current(void);
uint32_t x86_current_cpu_index(void);
uint32_t x86_current_apic_id(void);
vaddr_t x86_cpu_kernel_stack(uint32_t cpu_index);
void x86_cpu_note_user_entry(void);
uint64_t x86_cpu_user_entry_count(uint32_t cpu_index);
void x86_tss_set_rsp0(vaddr_t stack_top);
vaddr_t x86_tss_get_rsp0(void);
void x86_set_user_fs_base(vaddr_t fs_base);
uint64_t x86_read_tsc(void);
uint64_t x86_timeout_ns_to_tsc(uint64_t timeout_ns);
uint64_t x86_tsc_to_ns(uint64_t tsc);

/* 现有启动路径的兼容接口。 */
BOOLEAN liteos_arch_cpu_init(void);
BOOLEAN liteos_arch_set_kernel_stack(UINT64 stack_base, UINT64 stack_size);

#endif
