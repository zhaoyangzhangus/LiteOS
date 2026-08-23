#include <arch/x86_64/cpu.h>
#include <arch/x86_64/interrupt.h>
#include <arch/x86_64/paging.h>
#include <arch/x86_64/smp.h>
#include <arch/x86_64/uaccess.h>
#include <kernel/mm.h>
#include <kernel/irq.h>
#include "syscall.h"

#define IA32_EFER                 0xC0000080U
#define IA32_GS_BASE              0xC0000101U
#define IA32_KERNEL_GS_BASE       0xC0000102U
#define EFER_NXE                  (1ULL << 11)
#define CR0_WRITE_PROTECT         (1ULL << 16)
#define CR4_PCIDE                 (1ULL << 17)
#define CR4_SMEP                  (1ULL << 20)
#define CR4_SMAP                  (1ULL << 21)

#define LITEOS_GDT_CODE_SELECTOR  0x08U
#define LITEOS_IDT_ENTRIES        256U
#define LITEOS_IST_STACK_SIZE     (32U * 1024U)
#define LITEOS_RING0_STACK_RESERVE (64U * 1024U)

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} descriptor_table_register_t;

typedef struct __attribute__((packed)) {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attributes;
    uint16_t offset_middle;
    uint32_t offset_high;
    uint32_t reserved;
} idt_entry_t;

typedef struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb;
} x86_tss_t;

typedef void (*exception_entry_t)(void);
typedef void (*irq_entry_t)(void);

/*
 * 每个逻辑 CPU 都拥有独立描述符表、TSS 和三条关键 IST 栈。BSP 的实例静态
 * 保留，以便物理内存分配器建立前即可使用；AP 实例在 vmalloc 可用后按需创建。
 */
typedef struct x86_cpu_arch {
    uint64_t gdt[7] __attribute__((aligned(8)));
    x86_tss_t tss __attribute__((aligned(16)));
    idt_entry_t idt[LITEOS_IDT_ENTRIES] __attribute__((aligned(16)));
    uint8_t double_fault_stack[LITEOS_IST_STACK_SIZE] __attribute__((aligned(16)));
    uint8_t nmi_stack[LITEOS_IST_STACK_SIZE] __attribute__((aligned(16)));
    uint8_t machine_check_stack[LITEOS_IST_STACK_SIZE] __attribute__((aligned(16)));
    x86_cpu_local_t embedded_local __attribute__((aligned(CACHELINE_SIZE)));
    x86_cpu_local_t *local;
    uint32_t cpu_index;
    uint32_t apic_id;
    bool prepared;
} x86_cpu_arch_t;

static x86_cpu_arch_t g_bsp_arch __attribute__((aligned(CACHELINE_SIZE)));
static x86_cpu_arch_t *g_cpu_arch[MAX_CPUS];

x86_cpu_features_t x86_boot_cpu_features;

extern exception_entry_t liteos_exception_stub_table[32];
extern irq_entry_t liteos_irq_stub_table[IRQ_VECTOR_LAST - IRQ_VECTOR_FIRST + 1U];
extern void liteos_arch_interrupt_stub(void);
extern void liteos_lapic_timer_entry(void);
extern void liteos_lapic_spurious_entry(void);
extern void liteos_smp_ipi_entry(void);
extern void liteos_tlb_ipi_entry(void);

static void memory_zero(void *memory, size_t size) {
    uint8_t *bytes = (uint8_t *)memory;
    while (size-- != 0) *bytes++ = 0;
}

static void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *eax, uint32_t *ebx,
                  uint32_t *ecx, uint32_t *edx) {
    __asm__ volatile ("cpuid"
                      : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                      : "a"(leaf), "c"(subleaf));
}

static uint64_t read_msr(uint32_t index) {
    uint32_t low;
    uint32_t high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(index));
    return ((uint64_t)high << 32) | low;
}

static void write_msr(uint32_t index, uint64_t value) {
    __asm__ volatile ("wrmsr" : : "c"(index), "a"((uint32_t)value),
                      "d"((uint32_t)(value >> 32)) : "memory");
}

x86_cpu_local_t *x86_cpu_local_current(void) {
    x86_cpu_local_t *local;
    /* Self 位于固定偏移 40，避免通过全局变量猜测当前处理器。 */
    __asm__ volatile ("movq %%gs:40, %0" : "=r"(local));
    return local != 0 && local->Self == (uint64_t)(uintptr_t)local ? local : 0;
}

static x86_cpu_arch_t *current_arch(void) {
    x86_cpu_local_t *local = x86_cpu_local_current();
    if (local == 0 || local->CpuIndex >= MAX_CPUS) return 0;
    x86_cpu_arch_t *arch = g_cpu_arch[local->CpuIndex];
    return arch != 0 && arch->local == local ? arch : 0;
}

uint32_t x86_current_cpu_index(void) {
    x86_cpu_local_t *local = x86_cpu_local_current();
    return local != 0 ? local->CpuIndex : UINT32_MAX;
}

uint32_t x86_current_apic_id(void) {
    x86_cpu_local_t *local = x86_cpu_local_current();
    return local != 0 ? local->ApicId : UINT32_MAX;
}

vaddr_t x86_cpu_kernel_stack(uint32_t cpu_index) {
    if (cpu_index >= MAX_CPUS || g_cpu_arch[cpu_index] == 0) return 0;
    return (vaddr_t)g_cpu_arch[cpu_index]->tss.rsp[0];
}

void x86_cpu_note_user_entry(void) {
    x86_cpu_local_t *local = x86_cpu_local_current();
    if (local != 0) __atomic_add_fetch(&local->UserEntries, 1ULL, __ATOMIC_RELAXED);
}

uint64_t x86_cpu_user_entry_count(uint32_t cpu_index) {
    if (cpu_index >= MAX_CPUS || g_cpu_arch[cpu_index] == 0 ||
        g_cpu_arch[cpu_index]->local == 0) return 0;
    return __atomic_load_n(&g_cpu_arch[cpu_index]->local->UserEntries, __ATOMIC_ACQUIRE);
}

uint32_t x86_cpu_preempt_disable_count(uint32_t cpu_index) {
    if (cpu_index >= MAX_CPUS ||
        g_cpu_arch[cpu_index] == 0 ||
        g_cpu_arch[cpu_index]->local == 0) {
        return UINT32_MAX;
    }

    return __atomic_load_n(
        &g_cpu_arch[cpu_index]->local->PreemptDisable,
        __ATOMIC_ACQUIRE);
}

void x86_cpu_detect_features(void) {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
    uint32_t maximum_basic;
    uint32_t maximum_extended;

    memory_zero(&x86_boot_cpu_features, sizeof(x86_boot_cpu_features));
    cpuid(0U, 0U, &maximum_basic, &ebx, &ecx, &edx);
    cpuid(0x80000000U, 0U, &maximum_extended, &ebx, &ecx, &edx);

    if (maximum_basic >= 1U) {
        cpuid(1U, 0U, &eax, &ebx, &ecx, &edx);
        x86_boot_cpu_features.pcid = (ecx & (1U << 17)) != 0;
        x86_boot_cpu_features.x2apic = (ecx & (1U << 21)) != 0;
        x86_boot_cpu_features.xsave = (ecx & (1U << 26)) != 0;
        x86_boot_cpu_features.tsc_deadline = (ecx & (1U << 24)) != 0;
    }
    if (maximum_basic >= 7U) {
        cpuid(7U, 0U, &eax, &ebx, &ecx, &edx);
        x86_boot_cpu_features.smep = (ebx & (1U << 7)) != 0;
        x86_boot_cpu_features.invpcid = (ebx & (1U << 10)) != 0;
        x86_boot_cpu_features.smap = (ebx & (1U << 20)) != 0;
        x86_boot_cpu_features.movdir64b = (ecx & (1U << 28)) != 0;
    }
    if (maximum_basic >= 0x15U) {
        uint32_t denominator;
        uint32_t numerator;
        uint32_t crystal_hz;
        cpuid(0x15U, 0U, &denominator, &numerator, &crystal_hz, &edx);
        if (denominator != 0 && numerator != 0 && crystal_hz != 0) {
            x86_boot_cpu_features.tsc_hz =
                ((uint64_t)crystal_hz * numerator) / denominator;
        }
    }
    if (x86_boot_cpu_features.tsc_hz == 0 && maximum_basic >= 0x16U) {
        uint32_t base_mhz;
        cpuid(0x16U, 0U, &base_mhz, &ebx, &ecx, &edx);
        if (base_mhz != 0) x86_boot_cpu_features.tsc_hz = (uint64_t)base_mhz * 1000000ULL;
    }
    if (x86_boot_cpu_features.tsc_hz == 0) {
        /* 极早期平台缺少频率叶时使用保守后备值，后续由时钟源校准覆盖。 */
        x86_boot_cpu_features.tsc_hz = 1000000000ULL;
    }
    if (maximum_extended >= 0x80000001U) {
        cpuid(0x80000001U, 0U, &eax, &ebx, &ecx, &edx);
        x86_boot_cpu_features.nx = (edx & (1U << 20)) != 0;
    }
    if (maximum_extended >= 0x80000007U) {
        cpuid(0x80000007U, 0U, &eax, &ebx, &ecx, &edx);
        x86_boot_cpu_features.invariant_tsc = (edx & (1U << 8)) != 0;
    }
    x86_boot_cpu_features.phys_bits = 36U;
    x86_boot_cpu_features.virt_bits = 48U;
    if (maximum_extended >= 0x80000008U) {
        cpuid(0x80000008U, 0U, &eax, &ebx, &ecx, &edx);
        x86_boot_cpu_features.phys_bits = (uint8_t)(eax & 0xFFU);
        x86_boot_cpu_features.virt_bits = (uint8_t)((eax >> 8) & 0xFFU);
    }
}

uint64_t x86_read_tsc(void) {
    uint32_t low;
    uint32_t high;
    __asm__ volatile ("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32) | low;
}

uint64_t x86_timeout_ns_to_tsc(uint64_t timeout_ns) {
    if (timeout_ns == UINT64_MAX) return UINT64_MAX;
    uint64_t hz = x86_boot_cpu_features.tsc_hz;
    uint64_t seconds = timeout_ns / 1000000000ULL;
    uint64_t remainder = timeout_ns % 1000000000ULL;
    if (seconds > UINT64_MAX / hz) return UINT64_MAX;
    uint64_t ticks = seconds * hz;
    uint64_t hz_whole = hz / 1000000000ULL;
    uint64_t hz_remainder = hz % 1000000000ULL;
    uint64_t fraction = remainder * hz_whole;
    uint64_t product = remainder * hz_remainder;
    fraction += product / 1000000000ULL;
    if (product % 1000000000ULL != 0) ++fraction;
    return fraction > UINT64_MAX - ticks ? UINT64_MAX : ticks + fraction;
}

uint64_t x86_tsc_to_ns(uint64_t tsc) {
    uint64_t hz = x86_boot_cpu_features.tsc_hz;
    uint64_t seconds = tsc / hz;
    uint64_t remainder = tsc % hz;
    if (seconds > UINT64_MAX / 1000000000ULL ||
        remainder > UINT64_MAX / 1000000000ULL) return UINT64_MAX;
    return seconds * 1000000000ULL + (remainder * 1000000000ULL) / hz;
}

void x86_cpu_enable_protection(void) {
    uint64_t cr0;
    uint64_t cr4;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr0 |= CR0_WRITE_PROTECT;
    if (x86_boot_cpu_features.pcid && x86_boot_cpu_features.invpcid) {
        cr4 |= CR4_PCIDE;
    }
    if (x86_boot_cpu_features.smep) cr4 |= CR4_SMEP;
    if (x86_boot_cpu_features.smap) cr4 |= CR4_SMAP;
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0) : "memory");
    __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4) : "memory");
    if (x86_boot_cpu_features.nx) write_msr(IA32_EFER, read_msr(IA32_EFER) | EFER_NXE);
    x86_uaccess_init(x86_boot_cpu_features.smap);
}

uint32_t x86_cpu_protection_status(void) {
    uint64_t cr0;
    uint64_t cr4;
    uint64_t efer = 0;
    uint32_t status = 0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    if ((cr0 & CR0_WRITE_PROTECT) != 0) status |= X86_PROTECTION_WRITE_PROTECT;
    if (x86_boot_cpu_features.nx) efer = read_msr(IA32_EFER);
    if ((efer & EFER_NXE) != 0) status |= X86_PROTECTION_NX;
    if (x86_boot_cpu_features.smep && (cr4 & CR4_SMEP) != 0) {
        status |= X86_PROTECTION_SMEP;
    }
    if (x86_boot_cpu_features.smap && (cr4 & CR4_SMAP) != 0) {
        status |= X86_PROTECTION_SMAP;
    }
    return status;
}

bool x86_cpu_hardening_self_test(void) {
    uint32_t required = X86_PROTECTION_WRITE_PROTECT;
    if (x86_boot_cpu_features.nx) required |= X86_PROTECTION_NX;
    if (x86_boot_cpu_features.smep) required |= X86_PROTECTION_SMEP;
    if (x86_boot_cpu_features.smap) required |= X86_PROTECTION_SMAP;
    return (x86_cpu_protection_status() & required) == required;
}

void x86_tss_set_rsp0(vaddr_t stack_top) {
    x86_cpu_arch_t *arch = current_arch();
    if (stack_top != 0 && (stack_top & 15U) == 0 && x86_is_canonical(stack_top)) {
        if (arch != 0) arch->tss.rsp[0] = stack_top;
    }
}

vaddr_t x86_tss_get_rsp0(void) {
    x86_cpu_arch_t *arch = current_arch();
    return arch != 0 ? (vaddr_t)arch->tss.rsp[0] : 0;
}

void x86_set_user_fs_base(vaddr_t fs_base) {
    if (fs_base <= X86_64_USER_TOP && x86_is_canonical(fs_base)) {
        write_msr(0xC0000100U, (uint64_t)fs_base); /* IA32_FS_BASE */
    }
}

static void load_gdt(const descriptor_table_register_t *table) {
    __asm__ volatile (
        "lgdt (%0)\n"
        "pushq $0x08\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "movw $0x10, %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%ss\n"
        "movw $0x28, %%ax\n"
        "ltr %%ax\n"
        :
        : "r"(table)
        : "rax", "memory");
}

static void set_idt_entry(idt_entry_t *entry, uintptr_t handler, uint8_t ist,
                          uint8_t type_attributes) {
    entry->offset_low = (uint16_t)(handler & 0xFFFFU);
    entry->selector = LITEOS_GDT_CODE_SELECTOR;
    entry->ist = ist & 7U;
    entry->type_attributes = type_attributes;
    entry->offset_middle = (uint16_t)((handler >> 16) & 0xFFFFU);
    entry->offset_high = (uint32_t)(handler >> 32);
    entry->reserved = 0;
}

static void build_idt(x86_cpu_arch_t *arch) {
    if (arch == 0) return;
    uintptr_t default_handler = (uintptr_t)&liteos_arch_interrupt_stub;
    for (uint32_t i = 0; i < LITEOS_IDT_ENTRIES; ++i) {
        set_idt_entry(&arch->idt[i], default_handler, 0U, 0x8EU);
    }
    for (uint32_t i = 0; i < 32U; ++i) {
        uint8_t ist = 0U;
        if (i == 8U) ist = X86_IST_DOUBLE_FAULT;
        else if (i == 2U) ist = X86_IST_NMI;
        else if (i == 18U) ist = X86_IST_MACHINE_CHECK;
        set_idt_entry(&arch->idt[i], (uintptr_t)liteos_exception_stub_table[i], ist,
                      i == 3U ? 0xEEU : 0x8EU);
    }
    for (uint32_t i = IRQ_VECTOR_FIRST; i <= IRQ_VECTOR_LAST; ++i) {
        set_idt_entry(&arch->idt[i],
                      (uintptr_t)liteos_irq_stub_table[i - IRQ_VECTOR_FIRST],
                      0U, 0x8EU);
    }
    set_idt_entry(&arch->idt[32], (uintptr_t)&liteos_lapic_timer_entry, 0U, 0x8EU);
    set_idt_entry(&arch->idt[X86_SMP_IPI_VECTOR], (uintptr_t)&liteos_smp_ipi_entry,
                  0U, 0x8EU);
    set_idt_entry(&arch->idt[X86_TLB_IPI_VECTOR], (uintptr_t)&liteos_tlb_ipi_entry,
                  0U, 0x8EU);
    set_idt_entry(&arch->idt[255], (uintptr_t)&liteos_lapic_spurious_entry, 0U, 0x8EU);
}

static void prepare_arch_context(x86_cpu_arch_t *arch, x86_cpu_local_t *local,
                                 uint32_t cpu_index, uint32_t apic_id,
                                 vaddr_t kernel_stack_top) {
    uintptr_t tss_base;
    uint64_t tss_limit = sizeof(arch->tss) - 1ULL;

    memory_zero(arch, sizeof(*arch));
    if (local != &arch->embedded_local) memory_zero(local, sizeof(*local));
    arch->local = local;
    arch->cpu_index = cpu_index;
    arch->apic_id = apic_id;

    /* GDT：空、内核代码、内核数据、用户数据、用户代码、16 字节 TSS。 */
    arch->gdt[1] = 0x00AF9A000000FFFFULL;
    arch->gdt[2] = 0x00CF92000000FFFFULL;
    arch->gdt[3] = 0x00CFF2000000FFFFULL;
    arch->gdt[4] = 0x00AFFA000000FFFFULL;

    arch->tss.rsp[0] = kernel_stack_top;
    arch->tss.ist[X86_IST_DOUBLE_FAULT - 1U] =
        (uintptr_t)arch->double_fault_stack + sizeof(arch->double_fault_stack);
    arch->tss.ist[X86_IST_NMI - 1U] =
        (uintptr_t)arch->nmi_stack + sizeof(arch->nmi_stack);
    arch->tss.ist[X86_IST_MACHINE_CHECK - 1U] =
        (uintptr_t)arch->machine_check_stack + sizeof(arch->machine_check_stack);
    arch->tss.iopb = sizeof(arch->tss);

    tss_base = (uintptr_t)&arch->tss;
    arch->gdt[5] = (tss_limit & 0xFFFFULL) |
                   ((tss_base & 0xFFFFFFULL) << 16) |
                   (0x89ULL << 40) |
                   (((tss_limit >> 16) & 0x0FULL) << 48) |
                   (((tss_base >> 24) & 0xFFULL) << 56);
    arch->gdt[6] = tss_base >> 32;
    build_idt(arch);

    local->KernelStack = kernel_stack_top;
    local->Self = (uint64_t)(uintptr_t)local;
    local->CpuIndex = cpu_index;
    local->ApicId = apic_id;
    arch->prepared = true;
}

static bool load_arch_context(x86_cpu_arch_t *arch) {
    if (arch == 0 || !arch->prepared || arch->local == 0) return false;
    descriptor_table_register_t gdt_register = {
        (uint16_t)(sizeof(arch->gdt) - 1U),
        (uint64_t)(uintptr_t)arch->gdt,
    };
    descriptor_table_register_t idt_register = {
        (uint16_t)(sizeof(arch->idt) - 1U),
        (uint64_t)(uintptr_t)arch->idt,
    };

    __asm__ volatile ("cli" : : : "memory");
    load_gdt(&gdt_register);
    __asm__ volatile ("lidt (%0)" : : "r"(&idt_register) : "memory");
    write_msr(IA32_GS_BASE, (uint64_t)(uintptr_t)arch->local);
    write_msr(IA32_KERNEL_GS_BASE, 0);
    x86_cpu_enable_protection();

    uint16_t task_selector;
    __asm__ volatile ("str %0" : "=r"(task_selector));
    return task_selector == 0x28U && x86_cpu_local_current() == arch->local;
}

void x86_idt_init(void) {
    x86_cpu_arch_t *arch = current_arch();
    if (arch == 0) return;
    build_idt(arch);
    descriptor_table_register_t idt_register = {
        (uint16_t)(sizeof(arch->idt) - 1U),
        (uint64_t)(uintptr_t)arch->idt,
    };
    __asm__ volatile ("lidt (%0)" : : "r"(&idt_register) : "memory");
}

BOOLEAN liteos_arch_cpu_init(void) {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
    x86_cpu_detect_features();
    cpuid(1U, 0U, &eax, &ebx, &ecx, &edx);
    (void)eax;
    (void)ecx;
    (void)edx;
    prepare_arch_context(&g_bsp_arch, &liteos_syscall_cpu_local, 0U, ebx >> 24, 0);
    g_cpu_arch[0] = &g_bsp_arch;
    if (!load_arch_context(&g_bsp_arch)) return 0;
    g_bsp_arch.local->Flags |= X86_CPU_LOCAL_ONLINE;
    return 1;
}

bool x86_cpu_bind_bootstrap(uint32_t cpu_index, uint32_t apic_id) {
    x86_cpu_local_t *local = x86_cpu_local_current();
    if (local == 0 || cpu_index >= MAX_CPUS || apic_id != local->ApicId) return false;
    uint32_t old_index = local->CpuIndex;
    if (old_index >= MAX_CPUS || g_cpu_arch[old_index] != &g_bsp_arch ||
        (g_cpu_arch[cpu_index] != 0 && g_cpu_arch[cpu_index] != &g_bsp_arch)) return false;
    if (old_index != cpu_index) g_cpu_arch[old_index] = 0;
    g_cpu_arch[cpu_index] = &g_bsp_arch;
    g_bsp_arch.cpu_index = cpu_index;
    g_bsp_arch.apic_id = apic_id;
    local->CpuIndex = cpu_index;
    return true;
}

bool x86_cpu_prepare_secondary(uint32_t cpu_index, uint32_t apic_id,
                               vaddr_t kernel_stack_top) {
    if (cpu_index >= MAX_CPUS || kernel_stack_top == 0 ||
        (kernel_stack_top & 15U) != 0 || !x86_is_canonical(kernel_stack_top) ||
        g_cpu_arch[cpu_index] != 0) return false;
    x86_cpu_arch_t *arch = (x86_cpu_arch_t *)vmalloc(sizeof(*arch));
    if (arch == 0) return false;
    prepare_arch_context(arch, &arch->embedded_local, cpu_index, apic_id,
                         kernel_stack_top);
    atomic_thread_fence(memory_order_release);
    g_cpu_arch[cpu_index] = arch;
    return true;
}

bool x86_cpu_init_secondary(uint32_t cpu_index, uint32_t apic_id) {
    if (cpu_index >= MAX_CPUS) return false;
    atomic_thread_fence(memory_order_acquire);
    x86_cpu_arch_t *arch = g_cpu_arch[cpu_index];
    if (arch == 0 || arch == &g_bsp_arch || arch->cpu_index != cpu_index ||
        arch->apic_id != apic_id || !load_arch_context(arch)) return false;
    return x86_current_cpu_index() == cpu_index && x86_current_apic_id() == apic_id;
}

BOOLEAN liteos_arch_set_kernel_stack(uint64_t stack_base, uint64_t stack_size) {
    if (stack_base == 0 || stack_size < LITEOS_RING0_STACK_RESERVE * 2ULL ||
        stack_base > UINT64_MAX - stack_size) {
        return 0;
    }
    /*
     * 当前引导线程仍在 BootstrapStack 的上半区保留 C 调用链。
     * Ring3 硬件中断使用下半区，避免覆盖稍后要恢复的返回地址。
     * 正式 thread_t 上线后，调度切换会把 RSP0 更新为该线程的专用内核栈。
     */
    uint64_t ring0_stack_top = stack_base + LITEOS_RING0_STACK_RESERVE;
    if ((ring0_stack_top & 0xFULL) != 0) return 0;
    x86_cpu_arch_t *arch = current_arch();
    if (arch == 0) return 0;
    arch->tss.rsp[0] = ring0_stack_top;
    arch->local->KernelStack = ring0_stack_top;
    return 1;
}
