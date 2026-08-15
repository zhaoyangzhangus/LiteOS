#include <arch/x86_64/acpi.h>
#include <arch/x86_64/apic.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/paging.h>
#include <arch/x86_64/smp.h>
#include <kernel/mm.h>
#include <kernel/sched.h>
#include "syscall.h"

enum {
    AP_STATE_OFFLINE = 0,
    AP_STATE_BOOTING,
    AP_STATE_STARTED,
    AP_STATE_ONLINE,
    AP_STATE_FAILED,
};

typedef struct __attribute__((packed)) {
    uint64_t cr3;
    uint64_t stack_top;
    uint64_t entry;
    uint32_t cpu_index;
    uint32_t apic_id;
    uint64_t generation;
} x86_ap_trampoline_parameters_t;

_Static_assert(sizeof(x86_ap_trampoline_parameters_t) == 40U,
               "AP trampoline parameter layout");
_Static_assert(__builtin_offsetof(x86_ap_trampoline_parameters_t, cpu_index) == 24U,
               "AP trampoline cpu index offset");

extern const uint8_t x86_ap_trampoline_start[];
extern const uint8_t x86_ap_trampoline_parameters[];
extern const uint8_t x86_ap_trampoline_end[];

static atomic_uint g_ap_states[MAX_CPUS];
static void *g_ap_stacks[MAX_CPUS];
static atomic_uint g_started_count;
static atomic_uint g_online_count;
static atomic_bool g_release_aps;
static atomic_uint_fast64_t g_ipi_acknowledgements[MAX_CPUS];
static atomic_bool g_reschedule_pending[MAX_CPUS];
static atomic_uint_fast64_t g_online_words[MAX_CPUS / 64U];
static uint32_t g_discovered_count;

static void bytes_zero(void *memory, size_t size) {
    uint8_t *bytes = (uint8_t *)memory;
    while (size-- != 0) *bytes++ = 0;
}

static void bytes_copy(void *destination, const void *source, size_t size) {
    uint8_t *out = (uint8_t *)destination;
    const uint8_t *in = (const uint8_t *)source;
    while (size-- != 0) *out++ = *in++;
}

static void tsc_delay(uint64_t nanoseconds) {
    uint64_t start = x86_read_tsc();
    uint64_t ticks = x86_timeout_ns_to_tsc(nanoseconds);
    uint64_t deadline = ticks > UINT64_MAX - start ? UINT64_MAX : start + ticks;
    while ((int64_t)(x86_read_tsc() - deadline) < 0) __asm__ volatile ("pause");
}

static bool wait_for_ap(uint32_t cpu_index, uint64_t timeout_ns) {
    uint64_t start = x86_read_tsc();
    uint64_t ticks = x86_timeout_ns_to_tsc(timeout_ns);
    uint64_t deadline = ticks > UINT64_MAX - start ? UINT64_MAX : start + ticks;
    for (;;) {
        unsigned state = atomic_load_explicit(&g_ap_states[cpu_index],
                                              memory_order_acquire);
        if (state == AP_STATE_STARTED || state == AP_STATE_ONLINE) return true;
        if (state == AP_STATE_FAILED || (int64_t)(x86_read_tsc() - deadline) >= 0) {
            return false;
        }
        __asm__ volatile ("pause");
    }
}

static uint32_t current_legacy_apic_id(void) {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                      : "a"(1U), "c"(0U));
    (void)eax;
    (void)ecx;
    (void)edx;
    return ebx >> 24;
}

static bool ap_features_match(void) {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
    uint32_t maximum_basic;
    uint32_t maximum_extended;
    __asm__ volatile ("cpuid" : "=a"(maximum_basic), "=b"(ebx),
                      "=c"(ecx), "=d"(edx) : "a"(0U), "c"(0U));
    __asm__ volatile ("cpuid" : "=a"(maximum_extended), "=b"(ebx),
                      "=c"(ecx), "=d"(edx) : "a"(0x80000000U), "c"(0U));
    if (maximum_extended < 0x80000001U) return !x86_boot_cpu_features.nx;
    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                      : "a"(0x80000001U), "c"(0U));
    if (((edx & (1U << 20)) != 0) != x86_boot_cpu_features.nx) return false;
    if (maximum_basic >= 7U) {
        __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                          : "a"(7U), "c"(0U));
        if (((ebx & (1U << 7)) != 0) != x86_boot_cpu_features.smep ||
            ((ebx & (1U << 20)) != 0) != x86_boot_cpu_features.smap) return false;
    } else if (x86_boot_cpu_features.smep || x86_boot_cpu_features.smap) {
        return false;
    }
    if (maximum_basic >= 1U) {
        __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                          : "a"(1U), "c"(0U));
        if (((ecx & (1U << 17)) != 0) != x86_boot_cpu_features.pcid) return false;
    } else if (x86_boot_cpu_features.pcid) {
        return false;
    }
    if (maximum_extended >= 0x80000008U) {
        __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                          : "a"(0x80000008U), "c"(0U));
        if ((uint8_t)eax != x86_boot_cpu_features.phys_bits ||
            (uint8_t)(eax >> 8) != x86_boot_cpu_features.virt_bits) return false;
    }
    return true;
}

__noreturn void x86_smp_ap_entry(uint32_t cpu_index) {
    const x86_acpi_platform_t *platform = x86_acpi_platform();
    bool valid = platform != 0 && cpu_index < platform->cpu_count &&
                 platform->cpus[cpu_index].apic_id == current_legacy_apic_id() &&
                 ap_features_match();
    uint32_t apic_id = valid ? platform->cpus[cpu_index].apic_id : UINT32_MAX;
    if (valid) valid = x86_cpu_init_secondary(cpu_index, apic_id);
    if (valid) {
        uint64_t stack_top = (uint64_t)(uintptr_t)g_ap_stacks[cpu_index] +
                             X86_AP_BOOT_STACK_SIZE;
        valid = liteos_syscall_init(stack_top) &&
                liteos_lapic_init_secondary(apic_id, 32U, 1000000U);
    }
    if (valid) {
        valid = x86_cpu_local_current() != 0;
    }
    atomic_store_explicit(&g_ap_states[cpu_index],
                          valid ? AP_STATE_STARTED : AP_STATE_FAILED,
                          memory_order_release);
    if (valid) atomic_fetch_add_explicit(&g_started_count, 1U, memory_order_relaxed);

    if (valid) {
        /* BSP 建立全部运行队列后才发布 online，避免调度器观察到半初始化 CPU。 */
        while (!atomic_load_explicit(&g_release_aps, memory_order_acquire)) {
            __asm__ volatile ("pause");
        }
        x86_cpu_local_t *local = x86_cpu_local_current();
        local->Flags |= X86_CPU_LOCAL_ONLINE;
        atomic_fetch_or_explicit(&g_online_words[cpu_index >> 6],
                                 1ULL << (cpu_index & 63U), memory_order_release);
        atomic_fetch_add_explicit(&g_online_count, 1U, memory_order_release);
        atomic_store_explicit(&g_ap_states[cpu_index], AP_STATE_ONLINE,
                              memory_order_release);
        for (;;) {
            __asm__ volatile ("sti; hlt" : : : "memory");
            sched_finish_switch();
            /*
             * IPI 是低延迟唤醒路径，但不能成为正确性的唯一条件：
             * 若就绪线程恰好在 IPI 投递边界入队，空闲 CPU 仍必须主动
             * 扫描一次本地运行队列，避免跨 CPU 线程永久饥饿。
             */
            if (x86_smp_take_reschedule_request()) (void)sched_try_run_ready();
            __asm__ volatile ("cli" : : : "memory");
        }
    }
    for (;;) __asm__ volatile ("cli; hlt");
}

bool x86_smp_start_aps(const LITEOS_BOOT_INFO *boot_info) {
    const x86_acpi_platform_t *platform = x86_acpi_platform();
    if (boot_info == 0 || platform == 0 || platform->cpu_count == 0 ||
        boot_info->ApTrampolineSize != PAGE_SIZE ||
        (boot_info->ApTrampolineBase & (PAGE_SIZE - 1ULL)) != 0 ||
        boot_info->ApTrampolineBase < PAGE_SIZE ||
        boot_info->ApTrampolineBase > 0xFF000ULL) return false;

    size_t blob_size = (size_t)(x86_ap_trampoline_end - x86_ap_trampoline_start);
    size_t parameter_offset = (size_t)(x86_ap_trampoline_parameters -
                                       x86_ap_trampoline_start);
    if (blob_size == 0 || blob_size > PAGE_SIZE ||
        parameter_offset > blob_size - sizeof(x86_ap_trampoline_parameters_t)) return false;
    paddr_t root = x86_current_root_table();
    if (root.value == 0 || root.value > UINT32_MAX) return false;
    uint8_t *page = (uint8_t *)phys_to_direct(paddr_make(boot_info->ApTrampolineBase));
    if (page == 0) return false;
    bytes_zero(page, PAGE_SIZE);
    bytes_copy(page, x86_ap_trampoline_start, blob_size);
    x86_ap_trampoline_parameters_t *parameters =
        (x86_ap_trampoline_parameters_t *)(void *)(page + parameter_offset);

    uint32_t bsp_index = UINT32_MAX;
    for (uint32_t i = 0; i < platform->cpu_count; ++i) {
        if (platform->cpus[i].apic_id == platform->bsp_apic_id) {
            bsp_index = i;
            break;
        }
    }
    if (bsp_index == UINT32_MAX ||
        !x86_cpu_bind_bootstrap(bsp_index, platform->bsp_apic_id)) return false;

    g_discovered_count = platform->cpu_count;
    atomic_init(&g_started_count, 1U);
    atomic_init(&g_online_count, 1U);
    atomic_init(&g_release_aps, false);
    for (uint32_t word = 0; word < MAX_CPUS / 64U; ++word) {
        atomic_init(&g_online_words[word], 0);
    }
    atomic_fetch_or_explicit(&g_online_words[bsp_index >> 6],
                             1ULL << (bsp_index & 63U), memory_order_relaxed);
    for (uint32_t i = 0; i < platform->cpu_count; ++i) {
        atomic_init(&g_ap_states[i], platform->cpus[i].apic_id == platform->bsp_apic_id ?
                                      AP_STATE_STARTED : AP_STATE_OFFLINE);
        atomic_init(&g_ipi_acknowledgements[i], 0);
        atomic_init(&g_reschedule_pending[i], false);
        g_ap_stacks[i] = 0;
    }

    uint8_t vector = (uint8_t)(boot_info->ApTrampolineBase >> PAGE_SHIFT);
    uint64_t generation = 1U;
    for (uint32_t i = 0; i < platform->cpu_count; ++i) {
        uint32_t apic_id = platform->cpus[i].apic_id;
        if (apic_id == platform->bsp_apic_id) continue;
        if (apic_id > 0xFFU || platform->cpus[i].x2apic) return false;
        void *stack = vmalloc(X86_AP_BOOT_STACK_SIZE);
        if (stack == 0) return false;
        g_ap_stacks[i] = stack;
        uint64_t stack_top = (uint64_t)(uintptr_t)stack + X86_AP_BOOT_STACK_SIZE;
        if (!x86_cpu_prepare_secondary(i, apic_id, stack_top)) return false;
        parameters->cr3 = root.value;
        parameters->stack_top = stack_top;
        parameters->entry = (uint64_t)(uintptr_t)&x86_smp_ap_entry;
        parameters->cpu_index = i;
        parameters->apic_id = apic_id;
        parameters->generation = generation++;
        atomic_store_explicit(&g_ap_states[i], AP_STATE_BOOTING, memory_order_release);
        atomic_thread_fence(memory_order_seq_cst);

        if (!liteos_lapic_send_init(apic_id)) return false;
        tsc_delay(10000000ULL);
        if (!liteos_lapic_send_startup(apic_id, vector)) return false;
        if (!wait_for_ap(i, 200000ULL)) {
            if (!liteos_lapic_send_startup(apic_id, vector) ||
                !wait_for_ap(i, 1000000000ULL)) return false;
        }
    }
    return atomic_load_explicit(&g_started_count, memory_order_acquire) ==
           platform->cpu_count;
}

uint32_t x86_smp_started_count(void) {
    return atomic_load_explicit(&g_started_count, memory_order_acquire);
}

uint32_t x86_smp_discovered_count(void) { return g_discovered_count; }

bool x86_smp_cpu_online(uint32_t cpu_index) {
    if (cpu_index >= g_discovered_count || cpu_index >= MAX_CPUS) return false;
    uint64_t word = atomic_load_explicit(&g_online_words[cpu_index >> 6],
                                         memory_order_acquire);
    return (word & (1ULL << (cpu_index & 63U))) != 0;
}

bool x86_smp_cpu_started(uint32_t cpu_index) {
    if (cpu_index >= g_discovered_count || cpu_index >= MAX_CPUS) return false;
    unsigned state = atomic_load_explicit(&g_ap_states[cpu_index], memory_order_acquire);
    return state == AP_STATE_STARTED || state == AP_STATE_ONLINE;
}

bool x86_smp_release_aps(void) {
    if (g_discovered_count == 0 ||
        atomic_load_explicit(&g_started_count, memory_order_acquire) !=
        g_discovered_count) return false;
    atomic_store_explicit(&g_release_aps, true, memory_order_release);

    uint64_t start = x86_read_tsc();
    uint64_t ticks = x86_timeout_ns_to_tsc(1000000000ULL);
    uint64_t deadline = ticks > UINT64_MAX - start ? UINT64_MAX : start + ticks;
    while (atomic_load_explicit(&g_online_count, memory_order_acquire) !=
           g_discovered_count) {
        if ((int64_t)(x86_read_tsc() - deadline) >= 0) return false;
        __asm__ volatile ("pause");
    }
    return true;
}

void x86_smp_ipi_interrupt(void) {
    uint32_t cpu_index = x86_current_cpu_index();
    bool reschedule = false;
    if (cpu_index < g_discovered_count && cpu_index < MAX_CPUS) {
        atomic_fetch_add_explicit(&g_ipi_acknowledgements[cpu_index], 1U,
                                  memory_order_release);
        reschedule = atomic_exchange_explicit(&g_reschedule_pending[cpu_index], false,
                                              memory_order_acq_rel);
    }
    liteos_lapic_end_of_interrupt();
    if (reschedule) {
        /* TLB shootdown 期间不能重入调度；保留请求，由下一次时钟继续处理。 */
        if (x86_tlb_shootdown_active() || sched_preempt_disabled()) {
            if (cpu_index < g_discovered_count && cpu_index < MAX_CPUS) {
                atomic_store_explicit(&g_reschedule_pending[cpu_index], true,
                                      memory_order_release);
            }
        } else {
            schedule();
        }
    }
}

bool x86_smp_ipi_self_test(void) {
    const x86_acpi_platform_t *platform = x86_acpi_platform();
    if (platform == 0 || platform->cpu_count != g_discovered_count ||
        atomic_load_explicit(&g_online_count, memory_order_acquire) !=
        g_discovered_count) return false;

    for (uint32_t cpu_index = 0; cpu_index < platform->cpu_count; ++cpu_index) {
        uint32_t apic_id = platform->cpus[cpu_index].apic_id;
        if (apic_id == platform->bsp_apic_id) continue;
        uint64_t before = atomic_load_explicit(&g_ipi_acknowledgements[cpu_index],
                                               memory_order_acquire);
        if (!liteos_lapic_send_fixed(apic_id, X86_SMP_IPI_VECTOR)) return false;

        uint64_t start = x86_read_tsc();
        uint64_t ticks = x86_timeout_ns_to_tsc(100000000ULL);
        uint64_t deadline = ticks > UINT64_MAX - start ? UINT64_MAX : start + ticks;
        while (atomic_load_explicit(&g_ipi_acknowledgements[cpu_index],
                                    memory_order_acquire) == before) {
            if ((int64_t)(x86_read_tsc() - deadline) >= 0) return false;
            __asm__ volatile ("pause");
        }
    }
    return true;
}

bool x86_smp_request_reschedule(uint32_t cpu_index) {
    const x86_acpi_platform_t *platform = x86_acpi_platform();
    if (platform == 0 || cpu_index >= platform->cpu_count ||
        !x86_smp_cpu_online(cpu_index)) return false;
    /* 同一目标 CPU 已有待处理请求时只保留一个 IPI，避免高频唤醒风暴。 */
    if (atomic_exchange_explicit(&g_reschedule_pending[cpu_index], true,
                                 memory_order_acq_rel)) return true;
    for (uint32_t attempt = 0; attempt < 4U; ++attempt) {
        if (liteos_lapic_send_fixed(platform->cpus[cpu_index].apic_id,
                                    X86_SMP_IPI_VECTOR)) return true;
        __asm__ volatile ("pause");
    }
    /* 请求保持为 pending，目标 CPU 的周期定时器仍会执行一次调度。 */
    return false;
}

bool x86_smp_take_reschedule_request(void) {
    uint32_t cpu_index = x86_current_cpu_index();
    if (cpu_index >= g_discovered_count || cpu_index >= MAX_CPUS) return false;
    return atomic_exchange_explicit(&g_reschedule_pending[cpu_index], false,
                                    memory_order_acq_rel);
}

bool x86_smp_remote_user_self_test(void) {
    if (g_discovered_count <= 1U) return true;
    uint32_t current_cpu = x86_current_cpu_index();
    for (uint32_t cpu_index = 0; cpu_index < g_discovered_count; ++cpu_index) {
        if (cpu_index != current_cpu && x86_smp_cpu_online(cpu_index) &&
            x86_cpu_user_entry_count(cpu_index) != 0) return true;
    }
    return false;
}
