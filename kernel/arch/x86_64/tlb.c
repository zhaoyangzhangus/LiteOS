#include <arch/x86_64/acpi.h>
#include <arch/x86_64/apic.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/paging.h>
#include <arch/x86_64/smp.h>
#include <kernel/mm.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>

/*
 * 当前实现串行化 shootdown 请求。这样页表写入者可以严格执行：发布 PTE 变更、
 * 远端失效、等待确认、最后复用物理页。以后可替换为每 CPU 请求队列而不改变接口。
 */
static spinlock_t g_tlb_shootdown_lock;
static atomic_uint_fast64_t g_tlb_generation;
static atomic_bool g_tlb_poisoned;
static atomic_uint_fast64_t g_tlb_acknowledgement[MAX_CPUS];
static atomic_bool g_tlb_waiting[MAX_CPUS];
static vaddr_t g_tlb_address;
static paddr_t g_tlb_root;
static uint16_t g_tlb_pcid;

typedef struct {
    uint64_t pcid;
    uint64_t address;
} invpcid_descriptor_t;

static void shootdown_lock(void) {
    uint32_t cpu_index = x86_current_cpu_index();
    uint64_t saved_flags = 0;
    bool enabled_delivery = false;
    while (atomic_exchange_explicit(&g_tlb_shootdown_lock.state, 1U,
                                     memory_order_acquire) != 0U) {
        if (!enabled_delivery) {
            __asm__ volatile ("pushfq; popq %0" : "=r"(saved_flags) : : "memory");
            if (cpu_index < MAX_CPUS) {
                atomic_store_explicit(&g_tlb_waiting[cpu_index], true,
                                      memory_order_release);
            }
            /* 目标 CPU 可能正在等待同一把锁；必须允许 TLB IPI 进来完成确认。 */
            __asm__ volatile ("sti" : : : "memory");
            enabled_delivery = true;
        }
        __asm__ volatile ("pause");
    }
    if (enabled_delivery) {
        if ((saved_flags & (1ULL << 9)) == 0) {
            __asm__ volatile ("cli" : : : "memory");
        }
        if (cpu_index < MAX_CPUS) {
            atomic_store_explicit(&g_tlb_waiting[cpu_index], false,
                                  memory_order_release);
        }
    }
}

static void shootdown_unlock(void) {
    atomic_store_explicit(&g_tlb_shootdown_lock.state, 0U, memory_order_release);
}

static void invalidate_local_page(vaddr_t address) {
    __asm__ volatile ("invlpg (%0)" : : "r"((uint64_t)address) : "memory");
}

static uint16_t root_pcid(paddr_t root) {
    page_t *root_page = phys_to_page(root);
    return root_page != 0 && root_page->owner == PAGE_OWNER_PAGETABLE ?
           (uint16_t)root_page->private_data : 0U;
}

static void invalidate_target_page(paddr_t root, uint16_t pcid, vaddr_t address) {
    if (x86_boot_cpu_features.pcid && x86_boot_cpu_features.invpcid) {
        invpcid_descriptor_t descriptor = {(uint64_t)pcid, (uint64_t)address};
        /* INVPCID type 0：只失效一个线性地址下的一个 PCID。 */
        __asm__ volatile ("invpcid %0, %1" : : "m"(descriptor), "r"(0ULL)
                          : "memory");
        return;
    }
    (void)root;
    invalidate_local_page(address);
}

static bool send_tlb_ipi_with_retry(uint32_t apic_id) {
    /* ICR 可能正被本地 APIC 的定时器/其它 IPI 占用，短暂失败时重试。 */
    for (uint32_t attempt = 0; attempt < 8U; ++attempt) {
        if (liteos_lapic_send_fixed(apic_id, X86_TLB_IPI_VECTOR)) return true;
        for (uint32_t pause = 0; pause < 256U; ++pause) {
            __asm__ volatile ("pause");
        }
    }
    return false;
}

bool x86_tlb_shootdown_active(void) {
    uint32_t cpu_index = x86_current_cpu_index();
    return cpu_index < MAX_CPUS &&
           atomic_load_explicit(&g_tlb_waiting[cpu_index], memory_order_acquire);
}

void x86_tlb_wait_begin(void) {
    uint32_t cpu_index = x86_current_cpu_index();
    if (cpu_index < MAX_CPUS) {
        atomic_store_explicit(&g_tlb_waiting[cpu_index], true,
                              memory_order_release);
    }
}

void x86_tlb_wait_end(void) {
    uint32_t cpu_index = x86_current_cpu_index();
    if (cpu_index < MAX_CPUS) {
        atomic_store_explicit(&g_tlb_waiting[cpu_index], false,
                              memory_order_release);
    }
}

static uint64_t tlb_enable_ipi_delivery(uint32_t cpu_index) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    if (cpu_index < MAX_CPUS) {
        atomic_store_explicit(&g_tlb_waiting[cpu_index], true, memory_order_release);
    }
    /* IF=0 的调用者可能持有 vmalloc 等不可抢占锁；远端 TLB IPI 不需要
       本 CPU 开中断，因此这里不能为了等待 shootdown 而打开定时器抢占。 */
    return flags;
}

static void tlb_restore_interrupt_state(uint32_t cpu_index, uint64_t flags) {
    if (cpu_index < MAX_CPUS) {
        atomic_store_explicit(&g_tlb_waiting[cpu_index], false, memory_order_release);
    }
    if ((flags & (1ULL << 9)) == 0) {
        __asm__ volatile ("cli" : : : "memory");
    }
}

static void tlb_finish_shootdown(uint32_t cpu_index, uint64_t flags) {
    tlb_restore_interrupt_state(cpu_index, flags);
    shootdown_unlock();
    /* TLB 期间延迟的 reschedule 必须在释放 shootdown 锁后立即消费。 */
        /* 重调度请求在后续定时器或空闲路径中消费，避免切换引导栈。 */
}

void x86_tlb_ipi_interrupt(void) {
    uint64_t generation = atomic_load_explicit(&g_tlb_generation,
                                               memory_order_acquire);
    vaddr_t address = g_tlb_address;
    paddr_t root = g_tlb_root;
    invalidate_target_page(root, g_tlb_pcid, address);
    uint32_t cpu_index = x86_current_cpu_index();
    if (generation != 0 && cpu_index < MAX_CPUS) {
        atomic_store_explicit(&g_tlb_acknowledgement[cpu_index], generation,
                              memory_order_release);
    }
    liteos_lapic_end_of_interrupt();
}

bool x86_tlb_shootdown_page(paddr_t root, vaddr_t virtual_address) {
    if (root.value == 0 || !x86_is_canonical((uint64_t)virtual_address)) return false;
    /* 一次失效超时后禁止继续释放/复用页面，避免延迟 IPI 污染下一次请求。 */
    if (atomic_load_explicit(&g_tlb_poisoned, memory_order_acquire)) return false;
    shootdown_lock();
    if (atomic_load_explicit(&g_tlb_poisoned, memory_order_acquire)) {
        shootdown_unlock();
        return false;
    }

    uint32_t current_cpu = x86_current_cpu_index();
    uint64_t interrupt_flags = tlb_enable_ipi_delivery(current_cpu);

    uint64_t generation = atomic_load_explicit(&g_tlb_generation,
                                               memory_order_relaxed) + 1U;
    if (generation == 0) generation = 1U;
    g_tlb_root = root;
    g_tlb_address = virtual_address;
    g_tlb_pcid = root_pcid(root);
    atomic_store_explicit(&g_tlb_generation, generation, memory_order_release);
    invalidate_target_page(root, g_tlb_pcid, virtual_address);

    const x86_acpi_platform_t *platform = x86_acpi_platform();
    if (platform != 0) {
        for (uint32_t cpu_index = 0; cpu_index < platform->cpu_count; ++cpu_index) {
            if (cpu_index == current_cpu || !x86_smp_cpu_online(cpu_index)) continue;
            if (!send_tlb_ipi_with_retry(platform->cpus[cpu_index].apic_id)) {
                atomic_store_explicit(&g_tlb_poisoned, true, memory_order_release);
                tlb_finish_shootdown(current_cpu, interrupt_flags);
                return false;
            }
        }

        uint64_t start = x86_read_tsc();
        uint64_t ticks = x86_timeout_ns_to_tsc(1000000000ULL);
        uint64_t deadline = ticks > UINT64_MAX - start ? UINT64_MAX : start + ticks;
        for (uint32_t cpu_index = 0; cpu_index < platform->cpu_count; ++cpu_index) {
            if (cpu_index == current_cpu || !x86_smp_cpu_online(cpu_index)) continue;
            while (atomic_load_explicit(&g_tlb_acknowledgement[cpu_index],
                                        memory_order_acquire) != generation) {
                if ((int64_t)(x86_read_tsc() - deadline) >= 0) {
                    atomic_store_explicit(&g_tlb_poisoned, true, memory_order_release);
                    tlb_finish_shootdown(current_cpu, interrupt_flags);
                    return false;
                }
                __asm__ volatile ("pause");
            }
        }
    }
    tlb_finish_shootdown(current_cpu, interrupt_flags);
    return true;
}

bool x86_tlb_shootdown_self_test(void) {
    /* 未映射地址上的 INVLPG 没有副作用，但仍会完整经过所有 online CPU。 */
    return x86_tlb_shootdown_page(x86_current_root_table(), (vaddr_t)0x7000U);
}
