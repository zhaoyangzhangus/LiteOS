#include <kernel/init_scheduler.h>

#include "arch/x86_64/paging.h"
#include <arch/x86_64/smp.h>
#include <kernel/debug_stage.h>
#include <kernel/handle.h>
#include <kernel/mutex.h>
#include <kernel/object.h>
#include <kernel/perf.h>
#include <kernel/process.h>
#include <kernel/realtest.h>
#include <kernel/sched.h>
#include <kernel/telemetry.h>
#include <kernel/vm.h>
#include <kernel/wait.h>

static BOOLEAN scheduler_fail_at(const liteos_init_scheduler_hooks_t *hooks,
                                 const CHAR8 *message, const char *file,
                                 uint32_t line) {
    liteos_debug_stage_fail_at(LITEOS_DEBUG_PHASE_SCHEDULER,
                               LITEOS_DEBUG_STEP_FAIL, K_EIO, file, line);
    hooks->write(message);
    hooks->halt();
    return 0;
}

#define scheduler_fail(hooks, message) \
    scheduler_fail_at((hooks), (message), __FILE__, __LINE__)

static BOOLEAN canonical_scheduler_self_test(void) {
    static thread_t fair_threads[300];
    thread_t thread;
    uint64_t benchmark_start;
    for (UINTN i = 0; i < sizeof(thread); ++i) ((UINT8 *)&thread)[i] = 0;
    atomic_init(&thread.state, THREAD_READY);
    atomic_init(&thread.blocked_waiter, 0);
    thread.sched_class = SCHED_CLASS_FAIR;
    thread.current_cpu = 0;
    list_init(&thread.sched.rt_node);
    benchmark_start = telemetry_timestamp();
    if (!sched_enqueue_bootstrap(&thread)) return 0;
    kernel_perf_emit_scope("scheduler.enqueue", benchmark_start);
    liteos_realtest_checkpoint("SCHED_CORE_STAGE_ENQUEUE_OK");
    benchmark_start = telemetry_timestamp();
    schedule();
    kernel_perf_emit_scope("scheduler.schedule", benchmark_start);
    if (atomic_load_explicit(&thread.state, memory_order_relaxed) != THREAD_RUNNING) return 0;
    liteos_realtest_checkpoint("SCHED_CORE_STAGE_SCHEDULE_OK");
    benchmark_start = telemetry_timestamp();
    sched_block_current();
    kernel_perf_emit_scope("scheduler.block", benchmark_start);
    if (atomic_load_explicit(&thread.state, memory_order_relaxed) != THREAD_BLOCKED) return 0;
    liteos_realtest_checkpoint("SCHED_CORE_STAGE_BLOCK_OK");

    benchmark_start = telemetry_timestamp();
    sched_wake(&thread);
    kernel_perf_emit_scope("scheduler.wake", benchmark_start);
    benchmark_start = telemetry_timestamp();
    schedule();
    kernel_perf_emit_scope("scheduler.wake_to_running", benchmark_start);
    if (atomic_load_explicit(&thread.state, memory_order_relaxed) != THREAD_RUNNING) return 0;
    liteos_realtest_checkpoint("SCHED_CORE_STAGE_WAKE_OK");
    /* Do not let the bootstrap fixture affect the fair-tree count below. */
    sched_block_current();
    if (atomic_load_explicit(&thread.state, memory_order_relaxed) != THREAD_BLOCKED) return 0;
    liteos_realtest_checkpoint("SCHED_CORE_STAGE_REBLOCK_OK");

    /* 瓒呰繃鏃ф暟缁勪笂闄愶紝骞朵互浜掔礌姝ラ暱涔卞簭鍒犻櫎锛岃鐩栫孩榛戞爲鍒犻櫎淇璺緞銆?*/
    for (UINT32 i = 0; i < 300U; ++i) {
        for (UINTN byte = 0; byte < sizeof(thread_t); ++byte) {
            ((UINT8 *)&fair_threads[i])[byte] = 0;
        }
        atomic_init(&fair_threads[i].state, THREAD_READY);
        atomic_init(&fair_threads[i].blocked_waiter, 0);
        fair_threads[i].tid = i + 1U;
        fair_threads[i].sched_class = SCHED_CLASS_FAIR;
        fair_threads[i].current_cpu = 0;
        fair_threads[i].sched.vruntime = (uint64_t)((i * 197U) % 307U);
        list_init(&fair_threads[i].sched.rt_node);
        sched_enqueue_bootstrap(&fair_threads[i]);
    }
    liteos_realtest_checkpoint("SCHED_CORE_STAGE_BULK_ENQUEUE_OK");
    if (sched_runnable_count() != 300U || !sched_validate_current_cpu()) return 0;
    liteos_realtest_checkpoint("SCHED_CORE_STAGE_VALIDATE_OK");
    liteos_realtest_checkpoint("SCHED_CORE_STAGE_REMOVE_BEGIN");
    for (UINT32 i = 0; i < 300U; ++i) {
        UINT32 index = (i * 73U) % 300U;
        benchmark_start = telemetry_timestamp();
        sched_remove(&fair_threads[index]);
        if (i == 0U) kernel_perf_emit_scope("scheduler.dequeue", benchmark_start);
        if ((i & 15U) == 0 && !sched_validate_current_cpu()) return 0;
        if ((i & 63U) == 63U) {
            liteos_realtest_checkpoint("SCHED_CORE_STAGE_REMOVE_PROGRESS");
        }
    }
    liteos_realtest_checkpoint("SCHED_CORE_STAGE_REMOVE_DONE");
    return sched_runnable_count() == 0 && sched_validate_current_cpu();
}

static BOOLEAN g_canonical_object_destroyed;

static VOID canonical_object_destroy(void *object) {
    (void)object;
    g_canonical_object_destroyed = 1;
}

static const object_ops_t g_canonical_object_ops = {
    .destroy = canonical_object_destroy,
    .type_name = "boot-test-object",
    .is_signaled = 0,
    .wait_value = 0,
};

static bool canonical_true_predicate(void *context) {
    return *(BOOLEAN *)context != 0;
}

static BOOLEAN canonical_object_handle_self_test(void) {
    struct {
        object_header_t header;
        UINT64 value;
    } object = {0};
    handle_table_t table;
    handle_t handle = 0;
    void *lookup = 0;
    BOOLEAN condition = 1;
    wait_queue_t queue;
    object.header.ops = &g_canonical_object_ops;
    object.header.type = 1U;
    refcount_init(&object.header.refs, 1U);
    g_canonical_object_destroyed = 0;
    if (handle_table_init(&table) != K_OK ||
        handle_create(&table, &object, 0x3U, &handle) != K_OK ||
        handle_lookup(&table, handle, 0x1U, &lookup) != K_OK || lookup != &object ||
        handle_close(&table, handle) != K_OK ||
        handle_lookup(&table, handle, 0U, &lookup) != K_ENOENT) return 0;
    object_put(lookup);
    object_put(&object);
    handle_table_destroy(&table);
    wait_queue_init(&queue);
    return g_canonical_object_destroyed &&
           wait_on_queue(&queue, canonical_true_predicate, &condition, 1000U) == K_OK;
}

static BOOLEAN canonical_vm_self_test(void) {
    vm_space_t *parent = 0;
    vm_space_t *child = 0;
    vm_object_t *object = 0;
    vm_object_t *shared_object = 0;
    vaddr_t address = 0x0000000040000000ULL;
    vaddr_t shared_parent_address = 0x0000000050000000ULL;
    vaddr_t shared_child_address = 0x0000000060000000ULL;
    paddr_t parent_physical = paddr_make(0);
    paddr_t child_physical = paddr_make(0);
    paddr_t shared_parent_physical = paddr_make(0);
    paddr_t shared_child_physical = paddr_make(0);
    uint64_t pte_flags = 0;
    uint64_t benchmark_start;
    kstatus_t benchmark_status;
    BOOLEAN success = 0;

    if (vm_space_create(&parent) != K_OK ||
        vm_object_create_anon(PAGE_SIZE * 3U, &object) != K_OK) goto cleanup;

    benchmark_start = telemetry_timestamp();
    benchmark_status = vm_map_object(
        parent, object, &address, 0, PAGE_SIZE * 3U,
        VM_PROT_READ | VM_PROT_WRITE | VM_PROT_USER,
        VM_MAP_PRIVATE | VM_MAP_FIXED);
    kernel_perf_emit_scope("mm.vma_insert", benchmark_start);
    if (benchmark_status != K_OK) goto cleanup;

    benchmark_start = telemetry_timestamp();
    benchmark_status = vm_handle_fault(
        parent, &(vm_fault_info_t){address, VM_PROT_WRITE, 0});
    kernel_perf_emit_scope("mm.page_fault", benchmark_start);
    if (benchmark_status != K_OK ||
        x86_translate_page(parent->root_table, address, &parent_physical, 0) != K_OK) goto cleanup;

    /* The page is now resident; this isolates the VMA tree lookup from the
     * first-fault allocation path. */
    benchmark_start = telemetry_timestamp();
    benchmark_status = vm_handle_fault(
        parent, &(vm_fault_info_t){address, VM_PROT_READ, 0});
    kernel_perf_emit_scope("mm.vma_lookup", benchmark_start);
    if (benchmark_status != K_OK) goto cleanup;

    UINT8 *parent_bytes = (UINT8 *)phys_to_direct(parent_physical);
    if (parent_bytes == 0) goto cleanup;
    parent_bytes[0] = 0x5AU;

    if (vm_space_clone_cow(parent, &child) != K_OK ||
        vm_handle_fault(child, &(vm_fault_info_t){address, VM_PROT_READ, 0}) != K_OK ||
        x86_translate_page(child->root_table, address, &child_physical, 0) != K_OK ||
        child_physical.value != parent_physical.value ||
        vm_handle_fault(child, &(vm_fault_info_t){address, VM_PROT_WRITE, 3U}) != K_OK ||
        x86_translate_page(child->root_table, address, &child_physical, 0) != K_OK ||
        child_physical.value == parent_physical.value) goto cleanup;

    UINT8 *child_bytes = (UINT8 *)phys_to_direct(child_physical);
    if (child_bytes == 0 || child_bytes[0] != 0x5AU) goto cleanup;
    child_bytes[0] = 0xA5U;
    if (parent_bytes[0] != 0x5AU) goto cleanup;

    vaddr_t middle = address + PAGE_SIZE;
    if (vm_handle_fault(parent, &(vm_fault_info_t){middle, VM_PROT_WRITE, 0}) != K_OK ||
        vm_protect(parent, middle, PAGE_SIZE, VM_PROT_READ | VM_PROT_USER) != K_OK ||
        x86_translate_page(parent->root_table, middle, &parent_physical, &pte_flags) != K_OK ||
        (pte_flags & 2U) != 0 ||
        vm_protect(parent, middle, PAGE_SIZE,
                   VM_PROT_READ | VM_PROT_WRITE | VM_PROT_USER) != K_OK ||
        vm_handle_fault(parent, &(vm_fault_info_t){middle, VM_PROT_WRITE, 3U}) != K_OK ||
        x86_translate_page(parent->root_table, middle, &parent_physical, &pte_flags) != K_OK ||
        (pte_flags & 2U) == 0) goto cleanup;

    benchmark_start = telemetry_timestamp();
    benchmark_status = vm_unmap(parent, middle, PAGE_SIZE);
    kernel_perf_emit_scope("mm.vma_remove", benchmark_start);
    if (benchmark_status != K_OK ||
        x86_translate_page(parent->root_table, middle, &parent_physical, 0) != K_ENOENT) goto cleanup;

    /* 鍏变韩瀵硅薄蹇呴』鍦ㄤ袱涓湴鍧€绌洪棿涓В鏋愬埌鍚屼竴鍚庡椤碉紝涓斿啓鍏ョ珛鍗冲彲瑙併€?*/
    if (vm_object_create_shared(PAGE_SIZE, &shared_object) != K_OK ||
        vm_map_object(parent, shared_object, &shared_parent_address, 0, PAGE_SIZE,
                      VM_PROT_READ | VM_PROT_WRITE | VM_PROT_USER,
                      VM_MAP_PRIVATE | VM_MAP_FIXED) != K_EINVAL ||
        vm_map_object(parent, shared_object, &shared_parent_address, 0, PAGE_SIZE,
                      VM_PROT_READ | VM_PROT_WRITE | VM_PROT_USER,
                      VM_MAP_SHARED | VM_MAP_FIXED) != K_OK ||
        vm_map_object(child, shared_object, &shared_child_address, 0, PAGE_SIZE,
                      VM_PROT_READ | VM_PROT_WRITE | VM_PROT_USER,
                      VM_MAP_SHARED | VM_MAP_FIXED) != K_OK ||
        vm_handle_fault(parent, &(vm_fault_info_t){shared_parent_address,
                                                   VM_PROT_WRITE, 0}) != K_OK ||
        vm_handle_fault(child, &(vm_fault_info_t){shared_child_address,
                                                  VM_PROT_READ, 0}) != K_OK ||
        x86_translate_page(parent->root_table, shared_parent_address,
                           &shared_parent_physical, 0) != K_OK ||
        x86_translate_page(child->root_table, shared_child_address,
                           &shared_child_physical, 0) != K_OK ||
        shared_parent_physical.value != shared_child_physical.value) goto cleanup;
    UINT8 *shared_bytes = (UINT8 *)phys_to_direct(shared_parent_physical);
    UINT8 *shared_child_bytes = (UINT8 *)phys_to_direct(shared_child_physical);
    if (shared_bytes == 0 || shared_child_bytes == 0) goto cleanup;
    shared_bytes[0] = 0xC3U;
    if (shared_child_bytes[0] != 0xC3U) goto cleanup;
    success = 1;

cleanup:
    if (child != 0) vm_space_put(child);
    if (parent != 0) vm_space_put(parent);
    if (object != 0) vm_object_put(object);
    if (shared_object != 0) vm_object_put(shared_object);
    return success;
}

BOOLEAN liteos_init_scheduler(const LITEOS_BOOT_INFO *boot_info,
                              const liteos_init_scheduler_hooks_t *hooks) {
    uint64_t benchmark_start;
    if (boot_info == 0 || hooks == 0 || hooks->write == 0 ||
        hooks->write_u32 == 0 || hooks->halt == 0) return 0;

    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_4,
                       LITEOS_DEBUG_STEP_ENTER, 0U);
    liteos_realtest_mark("SMP_CALL_BEGIN");
    if (!x86_smp_start_aps(boot_info) ||
        x86_smp_started_count() != x86_smp_discovered_count()) {
        return scheduler_fail(hooks, "LITEOS_SMP_START_FAIL\r\n");
    }
    liteos_realtest_mark("SMP_CALL_OK");
    liteos_realtest_mark("SMP_REPORT_BEGIN");
    hooks->write("LITEOS_SMP_STARTED_COUNT=");
    hooks->write_u32(x86_smp_started_count());
    hooks->write("\r\nLITEOS_SMP_TRAMPOLINE_OK\r\n");
    liteos_realtest_mark("SMP_REPORT_OK");
    liteos_realtest_mark("SCHED_INIT_BEGIN");
    sched_init();
    liteos_realtest_mark("SCHED_INIT_OK");
    liteos_realtest_mark("SMP_RELEASE_BEGIN");
    if (!x86_smp_release_aps()) {
        return scheduler_fail(hooks, "LITEOS_SMP_RELEASE_FAIL\r\n");
    }
    liteos_realtest_mark("SMP_RELEASE_OK");
    liteos_realtest_mark("SMP_POST_RELEASE_REPORT_BEGIN");
    hooks->write("LITEOS_SMP_CPU_LOCAL_OK\r\n");
    liteos_realtest_mark("SMP_POST_RELEASE_REPORT_OK");
    liteos_realtest_mark("SMP_IPI_TEST_BEGIN");
    if (!x86_smp_ipi_self_test()) {
        return scheduler_fail(hooks, "LITEOS_SMP_IPI_FAIL\r\n");
    }
    liteos_realtest_mark("SMP_IPI_TEST_OK");
    hooks->write("LITEOS_SMP_IPI_OK\r\n");
    liteos_realtest_mark("SMP_TLB_TEST_BEGIN");
    benchmark_start = telemetry_timestamp();
    bool tlb_test_ok = x86_tlb_shootdown_self_test();
    kernel_perf_emit_scope("mm.tlb_shootdown", benchmark_start);
    if (!tlb_test_ok) {
        return scheduler_fail(hooks, "LITEOS_TLB_SHOOTDOWN_FAIL\r\n");
    }
    liteos_realtest_checkpoint("SMP_TLB_TEST_OK");
    hooks->write("LITEOS_TLB_SHOOTDOWN_OK\r\n");
    liteos_realtest_checkpoint("SCHED_SPEC4_READY_BEGIN");
    liteos_debug_stage_ready(LITEOS_DEBUG_PHASE_SPEC_4);
    liteos_realtest_checkpoint("SCHED_SPEC4_READY_OK");
    liteos_realtest_checkpoint("SCHED_SPEC5_ENTER_BEGIN");
    liteos_debug_stage_enter(LITEOS_DEBUG_PHASE_SPEC_5);
    liteos_realtest_checkpoint("SCHED_SPEC5_ENTER_OK");
    liteos_realtest_checkpoint("SCHED_CORE_TEST_BEGIN");
    benchmark_start = telemetry_timestamp();
    if (!canonical_scheduler_self_test()) {
        return scheduler_fail(hooks, "LITEOS_SCHED_CORE_FAIL\r\n");
    }
    liteos_realtest_checkpoint("SCHED_CORE_TEST_OK");
    liteos_realtest_checkpoint("SCHED_CORE_BENCH_BEGIN");
    kernel_perf_emit_scope("scheduler.core", benchmark_start);
    liteos_realtest_checkpoint("SCHED_CORE_BENCH_OK");
    liteos_realtest_checkpoint("SCHED_CORE_REPORT_BEGIN");
    hooks->write("LITEOS_SCHED_CORE_OK\r\n");
    liteos_realtest_checkpoint("SCHED_CORE_REPORT_OK");
    benchmark_start = telemetry_timestamp();
    if (!sched_accounting_self_test()) {
        return scheduler_fail(hooks, "LITEOS_SCHED_ACCOUNTING_FAIL\r\n");
    }
    kernel_perf_emit_scope("scheduler.accounting", benchmark_start);
    hooks->write("LITEOS_SCHED_ACCOUNTING_OK\r\n");
    benchmark_start = telemetry_timestamp();
    if (!sched_balance_self_test()) {
        return scheduler_fail(hooks, "LITEOS_SCHED_BALANCE_FAIL\r\n");
    }
    kernel_perf_emit_scope("scheduler.balance", benchmark_start);
    hooks->write("LITEOS_SCHED_BALANCE_OK\r\n");
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_4,
                       LITEOS_DEBUG_STEP_PROGRESS, 3U);
    if (!sched_state_transition_self_test()) {
        return scheduler_fail(hooks, "LITEOS_SCHED_STATE_TRANSITION_FAIL\r\n");
    }
    hooks->write("LITEOS_SCHED_STATE_TRANSITION_OK\r\n");
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_4,
                       LITEOS_DEBUG_STEP_PROGRESS, 4U);
    if (!sched_context_switch_self_test()) {
        return scheduler_fail(hooks, "LITEOS_CONTEXT_SWITCH_TEST_FAIL\r\n");
    }
    hooks->write("LITEOS_CONTEXT_SWITCH_OK\r\n");
    /* Idle CPUs poll pending wakeups before HLT to close the lost-IPI window. */
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_4,
                       LITEOS_DEBUG_STEP_PROGRESS, 6U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_SCHEDULER,
                       LITEOS_DEBUG_STEP_READY, 1U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_4,
                       LITEOS_DEBUG_STEP_PROGRESS, 2U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_4,
                       LITEOS_DEBUG_STEP_PROGRESS, 5U);
    if (!kmutex_pi_self_test()) {
        return scheduler_fail(hooks, "LITEOS_MUTEX_PI_FAIL\r\n");
    }
    hooks->write("LITEOS_MUTEX_PI_OK\r\n");
    liteos_debug_stage_ready(LITEOS_DEBUG_PHASE_SPEC_5);
    liteos_debug_stage_enter(LITEOS_DEBUG_PHASE_SPEC_6);
    liteos_debug_stage_enter(LITEOS_DEBUG_PHASE_SPEC_8);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_6,
                       LITEOS_DEBUG_STEP_ENTER, 0U);
    if (!canonical_object_handle_self_test()) {
        return scheduler_fail(hooks, "LITEOS_OBJECT_CORE_FAIL\r\n");
    }
    hooks->write("LITEOS_OBJECT_CORE_OK\r\n");
    benchmark_start = telemetry_timestamp();
    if (!canonical_vm_self_test()) {
        return scheduler_fail(hooks, "LITEOS_VM_CORE_FAIL\r\n");
    }
    kernel_perf_emit_scope("mm.vm", benchmark_start);
    hooks->write("LITEOS_VM_CORE_OK\r\nLITEOS_VM_SHARED_OK\r\n");
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_6,
                       LITEOS_DEBUG_STEP_PROGRESS, 7U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_6,
                       LITEOS_DEBUG_STEP_PROGRESS, 1U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_6,
                       LITEOS_DEBUG_STEP_PROGRESS, 2U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_6,
                       LITEOS_DEBUG_STEP_PROGRESS, 3U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_6,
                       LITEOS_DEBUG_STEP_PROGRESS, 4U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_6,
                       LITEOS_DEBUG_STEP_PROGRESS, 5U);
    /* Page-fault entry now hands CR2/error data to the MM policy owner. */
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_6,
                       LITEOS_DEBUG_STEP_PROGRESS, 6U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_2,
                       LITEOS_DEBUG_STEP_READY, 1U);
    benchmark_start = telemetry_timestamp();
    if (!process_core_self_test()) {
        return scheduler_fail(hooks, "LITEOS_PROCESS_CORE_FAIL\r\n");
    }
    kernel_perf_emit_scope("process.core", benchmark_start);
    hooks->write("LITEOS_PROCESS_CORE_OK\r\n");
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_5,
                       LITEOS_DEBUG_STEP_PROGRESS, 3U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_5,
                       LITEOS_DEBUG_STEP_PROGRESS, 4U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_5,
                       LITEOS_DEBUG_STEP_PROGRESS, 5U);
    liteos_debug_stage_ready(LITEOS_DEBUG_PHASE_SPEC_6);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_3,
                       LITEOS_DEBUG_STEP_PROGRESS, 6U);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_4,
                       LITEOS_DEBUG_STEP_PROGRESS, 1U);
    liteos_realtest_checkpoint("SMP_AP_TIMER_ENABLE_BEGIN");
    if (!x86_smp_enable_ap_timers()) {
        return scheduler_fail(hooks, "LITEOS_SMP_TIMER_ENABLE_FAIL\r\n");
    }
    liteos_realtest_mark("SMP_AP_TIMER_ENABLE_OK");
    return 1;
}
