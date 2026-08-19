#!/usr/bin/env python3
from pathlib import Path
import sys

ROOT = Path.cwd()

def read(rel):
    p = ROOT / rel
    if not p.exists():
        raise SystemExit(f"missing: {rel}")
    return p.read_text(encoding="utf-8")

def write(rel, text):
    (ROOT / rel).write_text(text, encoding="utf-8")

def replace_once(text, old, new, label):
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected exactly one match, found {count}")
    return text.replace(old, new, 1)

# ---------------------------------------------------------------------------
# 1) x86 kernel-thread first-entry trampoline
# ---------------------------------------------------------------------------
rel = "kernel/arch/x86_64/context.S"
s = read(rel)

s = replace_once(
    s,
    '''    .globl x86_switch_context_root
    .globl x86_rebase_stack_and_call
''',
    '''    .globl x86_switch_context_root
    .globl x86_kernel_thread_start
    .globl x86_rebase_stack_and_call
''',
    "context.S globals",
)

s = replace_once(
    s,
    '''    .def x86_switch_context_root; .scl 2; .type 32; .endef
    .def x86_rebase_stack_and_call; .scl 2; .type 32; .endef
''',
    '''    .def x86_switch_context_root; .scl 2; .type 32; .endef
    .def x86_kernel_thread_start; .scl 2; .type 32; .endef
    .def x86_rebase_stack_and_call; .scl 2; .type 32; .endef
''',
    "context.S defs",
)

anchor = '''/*
 * SysV 参数：RDI=虚拟地址增量，RSI=延续函数，RDX=延续函数参数。
'''
trampoline = '''/*
 * First entry for a pure Ring0 scheduler thread.
 *
 * Initial arch_switch_context_t convention:
 *   R12 = void (*entry)(void *)
 *   R13 = argument
 *
 * x86_switch_context_root() restores RSP/R12/R13 and executes ret into this
 * trampoline.  The saved stack is chosen so RSP is 16-byte aligned here;
 * CALL then gives the C entry the SysV-required RSP % 16 == 8.
 */
    .p2align 4
x86_kernel_thread_start:
    sti
    xorq %rbp, %rbp
    movq %r13, %rdi
    call *%r12

    /* Persistent kernel workers are not allowed to return. */
    cli
1:
    hlt
    jmp 1b

'''
if trampoline not in s:
    if anchor not in s:
        raise SystemExit("context.S trampoline anchor not found")
    s = s.replace(anchor, trampoline + anchor, 1)
write(rel, s)

# ---------------------------------------------------------------------------
# 2) public arch declaration
# ---------------------------------------------------------------------------
rel = "include/arch/x86_64/context.h"
s = read(rel)
old = '''void x86_switch_context_root(arch_switch_context_t *from,
                             const arch_switch_context_t *to,
                             paddr_t root);

/*
 * 将当前栈切换到同一物理内存的另一个虚拟别名，然后从一个全新的 C 调用链继续执行。
'''
new = '''void x86_switch_context_root(arch_switch_context_t *from,
                             const arch_switch_context_t *to,
                             paddr_t root);

/*
 * Pure Ring0 thread first-entry trampoline.
 * R12 carries entry, R13 carries its single void * argument.
 */
__attribute__((noreturn))
void x86_kernel_thread_start(void);

/*
 * 将当前栈切换到同一物理内存的另一个虚拟别名，然后从一个全新的 C 调用链继续执行。
'''
if "void x86_kernel_thread_start(void);" not in s:
    s = replace_once(s, old, new, "context.h declaration")
write(rel, s)

# ---------------------------------------------------------------------------
# 3) deferred API
# ---------------------------------------------------------------------------
rel = "include/kernel/deferred.h"
s = read(rel)
old = '''bool deferred_init(void);
bool deferred_schedule(deferred_work_fn_t function, void *argument);
'''
new = '''bool deferred_init(void);

/*
 * Start the persistent scheduler-visible Ring0 bottom-half worker.
 * deferred_init() stays early-boot safe; call this only after sched_init().
 */
bool deferred_start_worker(void);

bool deferred_schedule(deferred_work_fn_t function, void *argument);
'''
if "deferred_start_worker" not in s:
    s = replace_once(s, old, new, "deferred.h worker API")
write(rel, s)

# ---------------------------------------------------------------------------
# 4) deferred worker implementation
# ---------------------------------------------------------------------------
rel = "kernel/core/deferred.c"
s = read(rel)

old_includes = '''#include <kernel/deferred.h>
#include <kernel/rcu.h>
#include <kernel/spinlock.h>

#define DEFERRED_QUEUE_CAPACITY 128U
'''
new_includes = '''#include <arch/x86_64/context.h>
#include <arch/x86_64/cpu.h>
#include <kernel/deferred.h>
#include <kernel/process.h>
#include <kernel/rcu.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>

#define DEFERRED_QUEUE_CAPACITY 128U
#define DEFERRED_WORKER_STACK_SIZE (64U * 1024U)
'''
s = replace_once(s, old_includes, new_includes, "deferred.c includes")

old_struct_tail = '''    deferred_item_t critical_item;
    atomic_uint critical_state;
    atomic_uint init_state;
} g_deferred;
'''
new_struct_tail = '''    deferred_item_t critical_item;
    atomic_uint critical_state;
    atomic_uint init_state;

    /*
     * One global worker matches the current one-global-queue architecture.
     * It is a persistent Ring0 scheduler thread: no process, no user CR3.
     */
    thread_t worker;
    atomic_bool worker_started;
} g_deferred;

static uint8_t g_deferred_worker_stack[DEFERRED_WORKER_STACK_SIZE]
    __attribute__((aligned(16)));
'''
s = replace_once(s, old_struct_tail, new_struct_tail, "deferred.c worker storage")

unlock_anchor = '''static void deferred_unlock(uint64_t flags) {
    atomic_store_explicit(&g_deferred.lock.state, 0U, memory_order_release);
    deferred_irq_restore(flags);
}

'''
worker_helpers = r'''static void deferred_wake_worker(void) {
    if (!atomic_load_explicit(&g_deferred.worker_started,
                              memory_order_acquire)) {
        return;
    }

    /*
     * sched_wake() is IRQ-safe.  If the worker is still RUNNING this is only
     * a hint; the queue lock handshake below guarantees it will observe the
     * newly queued item before it can block.
     */
    sched_wake(&g_deferred.worker);
}

static bool deferred_worker_has_work_locked(void) {
    unsigned critical =
        atomic_load_explicit(&g_deferred.critical_state,
                             memory_order_acquire);

    return g_deferred.count != 0U ||
           critical == DEFERRED_CRITICAL_WRITING ||
           critical == DEFERRED_CRITICAL_READY;
}

static bool deferred_worker_prepare_sleep(void) {
    thread_t *thread = sched_current_thread();
    if (thread != &g_deferred.worker) return false;

    /*
     * Lost-wakeup barrier:
     *
     * Producer:
     *     deferred_lock -> enqueue/publish -> unlock -> sched_wake(worker)
     *
     * Consumer:
     *     deferred_lock -> verify empty -> RUNNING->BLOCKED -> unlock
     *                   -> schedule()
     *
     * Therefore an IRQ can never publish work in the gap between the empty
     * test and BLOCKED publication.  If it publishes after unlock,
     * sched_wake() sees BLOCKED (or READY/current) and preserves the wakeup.
     */
    uint64_t flags = deferred_lock();

    if (deferred_worker_has_work_locked()) {
        deferred_unlock(flags);
        return false;
    }

    unsigned expected = THREAD_RUNNING;
    bool blocked =
        atomic_compare_exchange_strong_explicit(
            &thread->state,
            &expected,
            THREAD_BLOCKED,
            memory_order_acq_rel,
            memory_order_acquire);

    deferred_unlock(flags);
    return blocked;
}

static void __attribute__((noreturn))
deferred_worker_main(void *argument) {
    (void)argument;

    for (;;) {
        /*
         * A bounded batch prevents a permanently busy device from keeping
         * this FAIR kernel worker on-CPU forever.
         */
        uint32_t completed = deferred_run(64U);

        if (completed == 64U) {
            schedule();
            continue;
        }

        /*
         * deferred_run() may have stopped because the queue became empty.
         * Recheck under the producer lock and publish BLOCKED atomically with
         * that empty observation.
         */
        if (deferred_worker_prepare_sleep()) {
            schedule();
        } else {
            __asm__ volatile ("pause");
        }
    }
}

'''
if "deferred_worker_prepare_sleep" not in s:
    s = replace_once(s, unlock_anchor, unlock_anchor + worker_helpers,
                     "deferred.c worker helpers")

old_init_tail = '''        g_deferred.critical_item.argument = 0;
        atomic_init(&g_deferred.critical_state, DEFERRED_CRITICAL_EMPTY);
        atomic_store_explicit(&g_deferred.init_state, 2U, memory_order_release);
        return true;
'''
new_init_tail = '''        g_deferred.critical_item.argument = 0;
        atomic_init(&g_deferred.critical_state, DEFERRED_CRITICAL_EMPTY);
        atomic_init(&g_deferred.worker_started, false);
        atomic_store_explicit(&g_deferred.init_state, 2U, memory_order_release);
        return true;
'''
s = replace_once(s, old_init_tail, new_init_tail, "deferred.c init worker flag")

init_end_anchor = '''    return true;
}

bool deferred_schedule(deferred_work_fn_t function, void *argument) {
'''
start_worker = r'''    return true;
}

bool deferred_start_worker(void) {
    if (!deferred_init()) return false;

    if (atomic_load_explicit(&g_deferred.worker_started,
                             memory_order_acquire)) {
        return true;
    }

    /*
     * This is the explicit scheduler-ready boundary.  Early boot calls only
     * deferred_init()/deferred_run(); they never need a schedulable thread.
     */
    thread_t *current = sched_current_thread();
    uint32_t cpu_id = x86_current_cpu_index();
    if (current == 0 || cpu_id >= MAX_CPUS) return false;

    thread_t *worker = &g_deferred.worker;
    uint8_t *worker_bytes = (uint8_t *)worker;
    for (size_t i = 0U; i < sizeof(*worker); ++i) {
        worker_bytes[i] = 0U;
    }

    refcount_init(&worker->object.refs, 1U);
    worker->object.type = KOBJECT_TYPE_THREAD;
    worker->object.flags = 0U;
    worker->object.ops = 0;
    worker->object.security = 0;

    /*
     * TID 0 is reserved from normal process allocation (user TIDs begin at
     * 1) and gives the bottom-half worker deterministic first ordering when
     * FAIR vruntime ties at boot.
     */
    worker->tid = 0U;
    worker->process = 0;
    atomic_init(&worker->state, THREAD_READY);

    worker->kernel_stack_base = g_deferred_worker_stack;
    worker->kernel_stack_size = sizeof(g_deferred_worker_stack);
    worker->kernel_stack_top =
        ((vaddr_t)(uintptr_t)g_deferred_worker_stack +
         sizeof(g_deferred_worker_stack)) &
        ~(vaddr_t)0x0FULL;

    worker->sched_class = SCHED_CLASS_FAIR;
    worker->base_sched_class = SCHED_CLASS_FAIR;
    worker->rt_priority = 0U;
    worker->base_rt_priority = 0U;
    worker->sched.weight = 1024U;
    worker->sched.nice = 0;
    worker->sched.vruntime = 0U;

    list_init(&worker->sched.rt_node);
    list_init(&worker->process_node);
    list_init(&worker->global_node);
    list_init(&worker->owned_mutexes);

    for (uint32_t word = 0U; word < MAX_CPUS / 64U; ++word) {
        worker->affinity.bits[word] = 0U;
    }
    worker->affinity.bits[cpu_id >> 6] =
        1ULL << (cpu_id & 63U);
    worker->current_cpu = (uint16_t)cpu_id;

    /*
     * x86_switch_context_root() finishes with RET.
     *
     * saved RSP points at the synthetic return address.  After RET the
     * trampoline sees a 16-byte-aligned RSP; its CALL then enters C with the
     * SysV-required RSP % 16 == 8.
     */
    uintptr_t stack_top = (uintptr_t)worker->kernel_stack_top;
    uintptr_t switch_stack = stack_top - sizeof(uint64_t);
    *(uint64_t *)switch_stack =
        (uint64_t)(uintptr_t)&x86_kernel_thread_start;

    worker->arch.switch_ctx.rsp = switch_stack;
    worker->arch.switch_ctx.r12 =
        (uint64_t)(uintptr_t)&deferred_worker_main;
    worker->arch.switch_ctx.r13 = 0U;
    worker->arch.switch_ctx.r14 = stack_top;
    worker->arch.fs_base = 0U;

    /*
     * Publish the wake target before making it runnable.  A concurrent IRQ
     * may call sched_wake() while state is READY; that is harmless because
     * sched_enqueue() below already preserves the runnable instance.
     */
    atomic_store_explicit(&g_deferred.worker_started, true,
                          memory_order_release);

    sched_enqueue(worker);

    /*
     * Force the first run now so the worker reaches its BLOCKED idle state
     * before Ring3 starts.  This also drains any xHCI/NVMe work queued before
     * sched_init().
     */
    if (!sched_try_run_ready()) {
        atomic_store_explicit(&g_deferred.worker_started, false,
                              memory_order_release);
        return false;
    }

    return true;
}

bool deferred_schedule(deferred_work_fn_t function, void *argument) {
'''
s = replace_once(s, init_end_anchor, start_worker, "deferred.c start worker")

old = '''    ++g_deferred.count;
    deferred_unlock(flags);
    return true;
}

bool deferred_try_schedule'''
new = '''    ++g_deferred.count;
    deferred_unlock(flags);
    deferred_wake_worker();
    return true;
}

bool deferred_try_schedule'''
s = replace_once(s, old, new, "deferred_schedule wake")

old = '''    ++g_deferred.count;
    deferred_unlock(flags);
    return true;
}

bool deferred_schedule_critical'''
new = '''    ++g_deferred.count;
    deferred_unlock(flags);
    deferred_wake_worker();
    return true;
}

bool deferred_schedule_critical'''
s = replace_once(s, old, new, "deferred_try_schedule wake")

old_critical = r'''bool deferred_schedule_critical(deferred_work_fn_t function, void *argument) {
    unsigned expected = DEFERRED_CRITICAL_EMPTY;
    /*
     * This single slot is deliberately reserved for the xHCI MSI-X worker.
     * Its caller owns a coalescing queued bit, so a READY/RUNNING item is
     * already sufficient to cover every later interrupt.  Do not turn this
     * into a shared fallback queue without adding per-source ownership.
     */
    if (function == 0 || !deferred_init()) return false;
    if (!atomic_compare_exchange_strong_explicit(&g_deferred.critical_state,
                                                 &expected,
                                                 DEFERRED_CRITICAL_WRITING,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        return false;
    }
    g_deferred.critical_item.function = function;
    g_deferred.critical_item.argument = argument;
    atomic_store_explicit(&g_deferred.critical_state, DEFERRED_CRITICAL_READY,
                          memory_order_release);
    return true;
}
'''
new_critical = r'''bool deferred_schedule_critical(deferred_work_fn_t function, void *argument) {
    unsigned expected = DEFERRED_CRITICAL_EMPTY;
    uint64_t flags;

    /*
     * This single slot is deliberately reserved for the xHCI MSI-X worker.
     * Its caller owns a coalescing queued bit, so a READY/RUNNING item is
     * already sufficient to cover every later interrupt.  Do not turn this
     * into a shared fallback queue without adding per-source ownership.
     *
     * Publication now shares deferred_lock with the worker's sleep handshake.
     * That is what closes the EMPTY->BLOCKED lost-wakeup window for the
     * emergency slot as well as the normal ring.
     */
    if (function == 0 || !deferred_init()) return false;

    flags = deferred_lock();

    if (!atomic_compare_exchange_strong_explicit(&g_deferred.critical_state,
                                                 &expected,
                                                 DEFERRED_CRITICAL_WRITING,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        deferred_unlock(flags);
        return false;
    }

    g_deferred.critical_item.function = function;
    g_deferred.critical_item.argument = argument;

    atomic_store_explicit(&g_deferred.critical_state,
                          DEFERRED_CRITICAL_READY,
                          memory_order_release);

    deferred_unlock(flags);
    deferred_wake_worker();
    return true;
}
'''
s = replace_once(s, old_critical, new_critical, "deferred critical wake")

write(rel, s)

# ---------------------------------------------------------------------------
# 5) start worker once canonical scheduler is fully self-tested
# ---------------------------------------------------------------------------
rel = "kernel/kernel_entry.c"
s = read(rel)

old = '''    if (!process_core_self_test()) {
        serial_write("LITEOS_PROCESS_CORE_FAIL\\r\\n");
        halt_forever();
    }
    serial_write("LITEOS_PROCESS_CORE_OK\\r\\n");
#if 0
'''
new = '''    if (!process_core_self_test()) {
        serial_write("LITEOS_PROCESS_CORE_FAIL\\r\\n");
        halt_forever();
    }
    serial_write("LITEOS_PROCESS_CORE_OK\\r\\n");

    /*
     * deferred_init() intentionally ran during early boot before the
     * canonical scheduler existed.  Now give the global deferred queue its
     * persistent Ring0 executor; IRQ producers no longer depend on kernel_main
     * reaching the idle HLT loop before bottom halves can run.
     */
    if (!deferred_start_worker()) {
        serial_write("LITEOS_DEFERRED_WORKER_FAIL\\r\\n");
        halt_forever();
    }
    serial_write("LITEOS_DEFERRED_WORKER_OK\\r\\n");
#if 0
'''
if "LITEOS_DEFERRED_WORKER_OK" not in s:
    s = replace_once(s, old, new, "kernel_entry deferred worker start")
write(rel, s)

print("FIX14 deferred worker applied.")
print("Modified:")
for rel in [
    "kernel/arch/x86_64/context.S",
    "include/arch/x86_64/context.h",
    "include/kernel/deferred.h",
    "kernel/core/deferred.c",
    "kernel/kernel_entry.c",
]:
    print("  ", rel)
print()
print("Next:")
print("  git diff --check")
print("  make clean && make")
print("  python3 tools/test_xhci_hub_hotplug.py")
