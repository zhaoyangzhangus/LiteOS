#include "arch/x86_64/apic.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/paging.h"
#include <arch/x86_64/smp.h>
#include <kernel/sched.h>
#include <kernel/process.h>
#include <uapi/process.h>
#include <kernel/deferred.h>
#include <kernel/timer.h>
#include <kernel/wait.h>
#include <kernel/watchdog.h>
#include <kernel/e1000.h>
#include <kernel/nvme_core.h>
#include <kernel/realtest.h>

#define LAPIC_ID_REGISTER            0x020U
#define LAPIC_VERSION_REGISTER       0x030U
#define LAPIC_SPURIOUS_REGISTER      0x0F0U
#define LAPIC_EOI_REGISTER           0x0B0U
#define LAPIC_LVT_TIMER_REGISTER     0x320U
#define LAPIC_ICR_LOW_REGISTER       0x300U
#define LAPIC_ICR_HIGH_REGISTER      0x310U
#define LAPIC_TIMER_DIVIDE_REGISTER  0x3E0U
#define LAPIC_TIMER_INITIAL_REGISTER 0x380U
#define LAPIC_TIMER_CURRENT_REGISTER 0x390U
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

static volatile UINT32 *g_lapic = (volatile UINT32 *)(uintptr_t)OS_LAPIC_BASE;
static volatile UINT64 g_tick_count;
static BOOLEAN g_initialized;
static UINT32 g_timer_initial_count;
#define LAPIC_SCHED_PERIOD_NS 2000000ULL
#define LAPIC_CALIBRATION_NS 5000000ULL
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
    if (!g_initialized || apic_id > 0xFFU) return 0;

    /*
     * ICR_HIGH and ICR_LOW are a single logical transaction.  In particular,
     * a timer IRQ may queue deferred work which sends a reschedule IPI while
     * this CPU is in a TLB shootdown.  Do not let a local interrupt overwrite
     * the destination between these two MMIO writes.  Keep the potentially
     * long delivery waits outside the IRQ-disabled window and recheck after
     * acquiring it in case an interrupt sent another IPI in the meantime.
     */
    UINT64 interrupt_flags;
    for (;;) {
        if (!lapic_wait_icr_idle()) return 0;
        __asm__ volatile ("pushfq; popq %0; cli"
                          : "=r"(interrupt_flags) : : "memory");
        if ((lapic_read(LAPIC_ICR_LOW_REGISTER) &
             LAPIC_ICR_DELIVERY_PENDING) == 0U) {
            break;
        }
        if ((interrupt_flags & (1ULL << 9)) != 0U) {
            __asm__ volatile ("sti" : : : "memory");
        }
    }
    lapic_write(LAPIC_ICR_HIGH_REGISTER, apic_id << 24);
    lapic_write(LAPIC_ICR_LOW_REGISTER, command);
    if ((interrupt_flags & (1ULL << 9)) != 0U) {
        __asm__ volatile ("sti" : : : "memory");
    }
    return lapic_wait_icr_idle();
}

BOOLEAN lapic_send_init(UINT32 apic_id) {
    return lapic_send_ipi(apic_id, LAPIC_DELIVERY_INIT |
                          LAPIC_ICR_LEVEL_ASSERT | LAPIC_ICR_TRIGGER_LEVEL);
}

BOOLEAN lapic_send_init_deassert(UINT32 apic_id) {
    return lapic_send_ipi(apic_id,
                          LAPIC_DELIVERY_INIT | LAPIC_ICR_TRIGGER_LEVEL);
}

BOOLEAN lapic_send_startup(UINT32 apic_id, UINT8 vector) {
    if (vector == 0) return 0;
    return lapic_send_ipi(apic_id, LAPIC_DELIVERY_STARTUP | vector);
}

BOOLEAN lapic_send_fixed(UINT32 apic_id, UINT8 vector) {
    if (vector < 32U) return 0;
    return lapic_send_ipi(apic_id, vector);
}

/* Calibrate the divided APIC clock against the already initialized TSC.
 * Fixed counts have no portable time unit (1,000,000 means 16ms in QEMU).
 * A masked one-shot prevents calibration from publishing timer work. */
static UINT32 lapic_calibrate_timer(UINT8 timer_vector, UINT32 fallback) {
    uint64_t window = x86_timeout_ns_to_tsc(LAPIC_CALIBRATION_NS);
    if (window == 0U) return fallback;
    lapic_write(LAPIC_LVT_TIMER_REGISTER,
                ((UINT32)timer_vector & 0xFFU) | LAPIC_TIMER_MASKED_BIT);
    lapic_write(LAPIC_TIMER_INITIAL_REGISTER, UINT32_MAX);
    uint64_t started = x86_read_tsc();
    UINT32 first = lapic_read(LAPIC_TIMER_CURRENT_REGISTER);
    uint64_t ended = started;
    for (uint32_t polls = 0U; polls < 1000000U; ++polls) {
        ended = x86_read_tsc();
        if (ended < started || ended - started >= window) break;
        __asm__ volatile("pause" : : : "memory");
    }
    UINT32 last = lapic_read(LAPIC_TIMER_CURRENT_REGISTER);
    if (ended < started || ended - started < window || first <= last)
        return fallback;
    uint64_t elapsed_ns = x86_tsc_to_ns(ended - started);
    if (elapsed_ns < LAPIC_CALIBRATION_NS || elapsed_ns > 100000000ULL)
        return fallback;
    /* A 32-bit APIC delta times 2ms cannot overflow uint64_t. */
    uint64_t count = (uint64_t)(first - last) * LAPIC_SCHED_PERIOD_NS / elapsed_ns;
    if (count == 0U || count > UINT32_MAX) return fallback;
    return (UINT32)count;
}

BOOLEAN lapic_init(UINT8 timer_vector, UINT32 initial_count) {
    if (g_initialized || timer_vector < 32U || initial_count == 0) return 0;
    /* 读取版本寄存器，确保恒等映射下的 LAPIC MMIO 区可访问。 */
    if (lapic_read(LAPIC_VERSION_REGISTER) == 0) return 0;
    lapic_write(LAPIC_SPURIOUS_REGISTER, LAPIC_ENABLE_BIT | 0xFFU);
    lapic_write(LAPIC_TIMER_DIVIDE_REGISTER, 0x3U);
    g_timer_initial_count = lapic_calibrate_timer(timer_vector, initial_count);
    lapic_write(LAPIC_LVT_TIMER_REGISTER,
                ((UINT32)timer_vector & 0xFFU) | LAPIC_TIMER_PERIODIC_BIT);
    atomic_init(&g_timer_maintenance_pending, false);
    atomic_init(&g_timer_maintenance_queued, false);
    lapic_write(LAPIC_TIMER_INITIAL_REGISTER, g_timer_initial_count);
    g_tick_count = 0;
    g_initialized = 1;
    return 1;
}

BOOLEAN lapic_init_secondary(UINT32 expected_apic_id, UINT8 timer_vector,
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
    /* All APs use the BSP's calibrated period, not the fallback argument. */
    lapic_write(LAPIC_TIMER_INITIAL_REGISTER, g_timer_initial_count);
    lapic_write(LAPIC_EOI_REGISTER, 0);
    return 1;
}

VOID lapic_enable_timer(void) {
    if (!g_initialized) return;
    lapic_write(LAPIC_LVT_TIMER_REGISTER,
                lapic_read(LAPIC_LVT_TIMER_REGISTER) &
                ~LAPIC_TIMER_MASKED_BIT);
}

BOOLEAN lapic_use_kernel_mapping(VOID) {
    paddr_t root = x86_current_root_table();
    paddr_t physical = paddr_make(OS_LAPIC_BASE);
    kstatus_t status = x86_map_page(root, (vaddr_t)LAPIC_KERNEL_VIRTUAL_BASE,
                                    physical, X86_PAGE_WRITE | X86_PAGE_GLOBAL,
                                    X86_CACHE_UC);
    if (status != K_OK && status != K_EBUSY) return 0;
    paddr_t translated;
    uint64_t flags;
    if (x86_translate_page(root, (vaddr_t)LAPIC_KERNEL_VIRTUAL_BASE,
                           &translated, &flags) != K_OK ||
        (translated.value & ~(PAGE_SIZE - 1ULL)) != OS_LAPIC_BASE) return 0;
    g_lapic = (volatile UINT32 *)(uintptr_t)LAPIC_KERNEL_VIRTUAL_BASE;
    return 1;
}

VOID lapic_end_of_interrupt(void) {
    if (g_initialized) lapic_write(LAPIC_EOI_REGISTER, 0);
}

UINT64 lapic_tick_count(void) {
    return __atomic_load_n(&g_tick_count, __ATOMIC_RELAXED);
}

UINT64 lapic_timer_interrupt(OS_INTERRUPT_CONTEXT *context) {
    (void)context;
    __atomic_add_fetch(&g_tick_count, 1ULL, __ATOMIC_RELAXED);
    /* 设备硬中断只排队；USB 事件和网络包在普通内核上下文处理。 */
    /* xHCI and E1000 input are interrupt-driven; timer IRQs never poll devices. */
    (void)nvme_schedule_deferred_poll();
    lapic_schedule_timer_maintenance();
    lapic_end_of_interrupt();
    uint64_t now_tsc = x86_read_tsc();
    sched_trace_wake_stall(now_tsc);
    /* deadline.S saves 15 GPRs before the hardware RIP/CS/RFLAGS frame. */
    const uint64_t *saved = (const uint64_t *)context;
    sched_trace_inbox_stall(now_tsc, saved != 0 ? saved[15] : 0U,
                            saved != 0 ? saved[17] : 0U);
    if (realtest_timeout_poll(now_tsc))
        realtest_timeout_reboot();
    /* 页表锁与 vmalloc 锁持有期间只接收 TLB IPI，不能在此重入调度。 */
    if (x86_tlb_shootdown_active()) return 0;
    /*
     * SYSCALL C code runs with IF=1 so it can receive TLB IPIs.  A timer
     * must not preempt that C call chain: its return frame lives on the
     * current thread's syscall stack and is resumed by the syscall stub.
     */
    if (x86_syscall_active_fast()) return 0;
    sched_tick(x86_tsc_to_ns(now_tsc));
    /* EOI is complete and CPL3 interrupted no kernel critical section.
     * Consume SIGKILL here too: a hung user loop may never make a syscall. */
    if (saved != 0 && (saved[16] & 3U) == 3U && !sched_preempt_disabled()) {
        thread_t *thread = sched_current_thread();
        if (thread != 0 && (__atomic_load_n(&thread->flags, __ATOMIC_ACQUIRE) & THREAD_FLAG_TERMINATE_REQUEST))
            thread_exit(-(int64_t)OS_SIGKILL);
        const uint64_t kill_bit = 1ULL << (OS_SIGKILL - 1U);
        if (thread != 0 && (__atomic_load_n(&thread->signal_pending, __ATOMIC_ACQUIRE) & kill_bit)) {
            __atomic_fetch_and(&thread->signal_pending, ~kill_bit, __ATOMIC_ACQ_REL);
            process_exit_signal(OS_SIGKILL);
        }
    }
    /* An IPI received in a syscall leaves its request pending. Remote
     * wakeups are still in the inbox, not nr_running, so the ordinary
     * timeslice test alone cannot discover them after syscall return.
     * Consume only at a safe boundary; never lose the request while a
     * map/allocator critical section has disabled preemption. */
    if (!sched_preempt_disabled() && x86_smp_take_reschedule_request()) {
        sched_handle_reschedule_request();
        return 0;
    }
    if (sched_tick_should_preempt()) schedule();
    return 0;
}
