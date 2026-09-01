#include "arch/x86_64/apic.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/paging.h"
#include <kernel/sched.h>
#include <kernel/deferred.h>
#include <kernel/timer.h>
#include <kernel/wait.h>
#include <kernel/watchdog.h>
#include <kernel/e1000.h>
#include <kernel/nvme_core.h>

#define LAPIC_ID_REGISTER            0x020U
#define LAPIC_VERSION_REGISTER       0x030U
#define LAPIC_SPURIOUS_REGISTER      0x0F0U
#define LAPIC_EOI_REGISTER           0x0B0U
#define LAPIC_LVT_TIMER_REGISTER     0x320U
#define LAPIC_ICR_LOW_REGISTER       0x300U
#define LAPIC_ICR_HIGH_REGISTER      0x310U
#define LAPIC_TIMER_DIVIDE_REGISTER  0x3E0U
#define LAPIC_TIMER_INITIAL_REGISTER 0x380U
#define LAPIC_ENABLE_BIT             (1U << 8)
#define LAPIC_TIMER_PERIODIC_BIT     (1U << 17)
#define LAPIC_TIMER_MASKED_BIT       (1U << 16)
#define LAPIC_ICR_DELIVERY_PENDING   (1U << 12)
#define LAPIC_ICR_LEVEL_ASSERT       (1U << 14)
#define LAPIC_ICR_TRIGGER_LEVEL      (1U << 15)
#define LAPIC_DELIVERY_INIT          (5U << 8)
#define LAPIC_DELIVERY_STARTUP       (6U << 8)
#define LAPIC_KERNEL_VIRTUAL_BASE    (X86_64_KERNEL_AUX_BASE + 0x00100000ULL)
#define IA32_APIC_BASE_MSR            0x1BU
#define IA32_APIC_GLOBAL_ENABLE       (1ULL << 11)

static volatile UINT32 *g_lapic = (volatile UINT32 *)(uintptr_t)LITEOS_LAPIC_BASE;
static volatile UINT64 g_tick_count;
static BOOLEAN g_initialized;
/*
 * Timer expiry, wait-queue expiry and watchdog expiry all take ordinary
 * kernel locks and can wake threads.  They must not run from the LAPIC hard
 * IRQ: an interrupt that arrives while its CPU owns one of those locks would
 * otherwise try to take the same lock recursively and spin forever.
 *
 * This is deliberately one coalesced maintenance item.  It is unrelated to
 * device polling: xHCI remains driven exclusively by its MSI-X handler.
 */
static atomic_bool g_timer_maintenance_pending;
static atomic_bool g_timer_maintenance_queued;

static void lapic_schedule_timer_maintenance(void);

static void lapic_timer_maintenance_work(void *argument) {
    uint64_t now_tsc;
    (void)argument;

    /* Consume the tick(s) which caused this work before examining deadlines. */
    (void)atomic_exchange_explicit(&g_timer_maintenance_pending, false,
                                   memory_order_acq_rel);
    now_tsc = x86_read_tsc();
    timer_poll(now_tsc);
    (void)watchdog_poll(now_tsc);
    wait_poll_timeouts(now_tsc);

    /*
     * Publish completion before testing pending.  If an IRQ races here it
     * either queues a successor itself, or leaves pending set for the call
     * below; no timeout tick is lost.
     */
    atomic_store_explicit(&g_timer_maintenance_queued, false,
                          memory_order_release);
    if (atomic_load_explicit(&g_timer_maintenance_pending,
                             memory_order_acquire)) {
        lapic_schedule_timer_maintenance();
    }
}

static void lapic_schedule_timer_maintenance(void) {
    bool expected = false;
    atomic_store_explicit(&g_timer_maintenance_pending, true,
                          memory_order_release);
    if (!atomic_compare_exchange_strong_explicit(&g_timer_maintenance_queued,
                                                 &expected, true,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        return;
    }
    if (!deferred_try_schedule(lapic_timer_maintenance_work, 0)) {
        /* Keep pending set; the next periodic timer IRQ will retry. */
        atomic_store_explicit(&g_timer_maintenance_queued, false,
                              memory_order_release);
    }
}

static UINT64 lapic_read_msr(UINT32 index) {
    UINT32 low;
    UINT32 high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(index));
    return ((UINT64)high << 32) | low;
}

static VOID lapic_write_msr(UINT32 index, UINT64 value) {
    __asm__ volatile ("wrmsr" : : "c"(index), "a"((UINT32)value),
                      "d"((UINT32)(value >> 32)) : "memory");
}

static VOID lapic_write(UINT32 offset, UINT32 value) {
    g_lapic[offset / sizeof(UINT32)] = value;
}

static UINT32 lapic_read(UINT32 offset) {
    return g_lapic[offset / sizeof(UINT32)];
}

static BOOLEAN lapic_wait_icr_idle(VOID) {
    uint64_t start = x86_read_tsc();
    uint64_t ticks = x86_timeout_ns_to_tsc(1000000ULL);
    uint64_t deadline = ticks > UINT64_MAX - start ? UINT64_MAX : start + ticks;
    while ((lapic_read(LAPIC_ICR_LOW_REGISTER) & LAPIC_ICR_DELIVERY_PENDING) != 0) {
        if ((int64_t)(x86_read_tsc() - deadline) >= 0) return 0;
        __asm__ volatile ("pause");
    }
    return 1;
}

static BOOLEAN lapic_send_ipi(UINT32 apic_id, UINT32 command) {
    if (!g_initialized || apic_id > 0xFFU || !lapic_wait_icr_idle()) return 0;
    lapic_write(LAPIC_ICR_HIGH_REGISTER, apic_id << 24);
    lapic_write(LAPIC_ICR_LOW_REGISTER, command);
    return lapic_wait_icr_idle();
}

BOOLEAN liteos_lapic_send_init(UINT32 apic_id) {
    return lapic_send_ipi(apic_id, LAPIC_DELIVERY_INIT |
                          LAPIC_ICR_LEVEL_ASSERT | LAPIC_ICR_TRIGGER_LEVEL);
}

BOOLEAN liteos_lapic_send_init_deassert(UINT32 apic_id) {
    return lapic_send_ipi(apic_id,
                          LAPIC_DELIVERY_INIT | LAPIC_ICR_TRIGGER_LEVEL);
}

BOOLEAN liteos_lapic_send_startup(UINT32 apic_id, UINT8 vector) {
    if (vector == 0) return 0;
    return lapic_send_ipi(apic_id, LAPIC_DELIVERY_STARTUP | vector);
}

BOOLEAN liteos_lapic_send_fixed(UINT32 apic_id, UINT8 vector) {
    if (vector < 32U) return 0;
    return lapic_send_ipi(apic_id, vector);
}

BOOLEAN liteos_lapic_init(UINT8 timer_vector, UINT32 initial_count) {
    if (g_initialized || timer_vector < 32U || initial_count == 0) return 0;
    /* 读取版本寄存器，确保恒等映射下的 LAPIC MMIO 区可访问。 */
    if (lapic_read(LAPIC_VERSION_REGISTER) == 0) return 0;
    lapic_write(LAPIC_SPURIOUS_REGISTER, LAPIC_ENABLE_BIT | 0xFFU);
    lapic_write(LAPIC_TIMER_DIVIDE_REGISTER, 0x3U);
    lapic_write(LAPIC_LVT_TIMER_REGISTER,
                ((UINT32)timer_vector & 0xFFU) | LAPIC_TIMER_PERIODIC_BIT);
    atomic_init(&g_timer_maintenance_pending, false);
    atomic_init(&g_timer_maintenance_queued, false);
    lapic_write(LAPIC_TIMER_INITIAL_REGISTER, initial_count);
    g_tick_count = 0;
    g_initialized = 1;
    return 1;
}

BOOLEAN liteos_lapic_init_secondary(UINT32 expected_apic_id, UINT8 timer_vector,
                                    UINT32 initial_count) {
    if (expected_apic_id > 0xFFU || timer_vector < 32U || initial_count == 0 ||
        !__atomic_load_n(&g_initialized, __ATOMIC_ACQUIRE)) return 0;

    UINT64 apic_base = lapic_read_msr(IA32_APIC_BASE_MSR);
    if ((apic_base & IA32_APIC_GLOBAL_ENABLE) == 0) {
        lapic_write_msr(IA32_APIC_BASE_MSR, apic_base | IA32_APIC_GLOBAL_ENABLE);
    }
    if (lapic_read(LAPIC_VERSION_REGISTER) == 0 ||
        (lapic_read(LAPIC_ID_REGISTER) >> 24) != expected_apic_id) return 0;

    /* IF 在运行队列建立前保持为零，因此可以先编程周期定时器再发布 online。 */
    lapic_write(LAPIC_SPURIOUS_REGISTER, LAPIC_ENABLE_BIT | 0xFFU);
    lapic_write(LAPIC_TIMER_DIVIDE_REGISTER, 0x3U);
    lapic_write(LAPIC_LVT_TIMER_REGISTER,
                ((UINT32)timer_vector & 0xFFU) | LAPIC_TIMER_PERIODIC_BIT |
                LAPIC_TIMER_MASKED_BIT);
    lapic_write(LAPIC_TIMER_INITIAL_REGISTER, initial_count);
    lapic_write(LAPIC_EOI_REGISTER, 0);
    return 1;
}

VOID liteos_lapic_enable_timer(void) {
    if (!g_initialized) return;
    lapic_write(LAPIC_LVT_TIMER_REGISTER,
                lapic_read(LAPIC_LVT_TIMER_REGISTER) &
                ~LAPIC_TIMER_MASKED_BIT);
}

BOOLEAN liteos_lapic_use_kernel_mapping(VOID) {
    paddr_t root = x86_current_root_table();
    paddr_t physical = paddr_make(LITEOS_LAPIC_BASE);
    kstatus_t status = x86_map_page(root, (vaddr_t)LAPIC_KERNEL_VIRTUAL_BASE,
                                    physical, X86_PAGE_WRITE | X86_PAGE_GLOBAL,
                                    X86_CACHE_UC);
    if (status != K_OK && status != K_EBUSY) return 0;
    paddr_t translated;
    uint64_t flags;
    if (x86_translate_page(root, (vaddr_t)LAPIC_KERNEL_VIRTUAL_BASE,
                           &translated, &flags) != K_OK ||
        (translated.value & ~(PAGE_SIZE - 1ULL)) != LITEOS_LAPIC_BASE) return 0;
    g_lapic = (volatile UINT32 *)(uintptr_t)LAPIC_KERNEL_VIRTUAL_BASE;
    return 1;
}

VOID liteos_lapic_end_of_interrupt(void) {
    if (g_initialized) lapic_write(LAPIC_EOI_REGISTER, 0);
}

UINT64 liteos_lapic_tick_count(void) {
    return __atomic_load_n(&g_tick_count, __ATOMIC_RELAXED);
}

UINT64 liteos_lapic_timer_interrupt(LITEOS_INTERRUPT_CONTEXT *context) {
    (void)context;
    __atomic_add_fetch(&g_tick_count, 1ULL, __ATOMIC_RELAXED);
    /* 设备硬中断只排队；USB 事件和网络包在普通内核上下文处理。 */
    /* xHCI and E1000 input are interrupt-driven; timer IRQs never poll devices. */
    (void)nvme_schedule_deferred_poll();
    lapic_schedule_timer_maintenance();
    liteos_lapic_end_of_interrupt();
    /* 页表锁与 vmalloc 锁持有期间只接收 TLB IPI，不能在此重入调度。 */
    if (x86_tlb_shootdown_active()) return 0;
    /*
     * SYSCALL C code runs with IF=1 so it can receive TLB IPIs.  A timer
     * must not preempt that C call chain: its return frame lives on the
     * current thread's syscall stack and is resumed by the syscall stub.
     */
    if (x86_syscall_active_fast()) return 0;
    uint64_t now_tsc = x86_read_tsc();
    sched_tick(x86_tsc_to_ns(now_tsc));
    return 0;
}
