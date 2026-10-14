#include <kernel/init_core.h>

#include <arch/x86_64/cpu.h>
#include <kernel/debug_stage.h>
#include <kernel/kmem.h>
#include <kernel/realtest.h>

static BOOLEAN core_fail_at(const kernel_init_core_hooks_t *hooks,
                            const CHAR8 *message, const char *file,
                            uint32_t line) {
    kernel_debug_stage_fail_at(OS_DEBUG_PHASE_SPEC_3,
                               OS_DEBUG_STEP_FAIL, K_EIO, file, line);
    hooks->write(message);
    hooks->halt();
    return 0;
}

#define core_fail(hooks, message) \
    core_fail_at((hooks), (message), __FILE__, __LINE__)

BOOLEAN kernel_init_core(const kernel_init_core_hooks_t *hooks) {
    if (hooks == 0 || hooks->write == 0 || hooks->halt == 0) return 0;

    kernel_debug_stage_enter(OS_DEBUG_PHASE_SPEC_3);
    realtest_mark("CORE_KMEM_BEGIN");
    if (!kmem_self_test()) {
        return core_fail(hooks, "OS_KMALLOC_TEST_FAIL\r\n");
    }
    hooks->write("OS_KMALLOC_OK\r\n");
    realtest_mark("CORE_KMEM_OK");
    realtest_mark("CORE_KMEM_STRESS_BEGIN");
    if (!kmem_stress_self_test()) {
        return core_fail(hooks, "OS_KMALLOC_STRESS_FAIL\r\n");
    }
    hooks->write("OS_KMALLOC_STRESS_OK\r\n");
    realtest_mark("CORE_KMEM_STRESS_OK");
    if (x86_current_cpu_index() < MAX_CPUS && kmem_fastpath_hits() == 0U) {
        return core_fail(hooks, "OS_KMALLOC_PERCPU_FAIL\r\n");
    }
    hooks->write("OS_KMALLOC_PERCPU_OK\r\n");
    realtest_mark("CORE_KMEM_PERCPU_OK");
    realtest_mark("CORE_VMALLOC_BEGIN");
    if (!vmalloc_self_test()) {
        return core_fail(hooks, "OS_VMALLOC_TEST_FAIL\r\n");
    }
    hooks->write("OS_VMALLOC_OK\r\n");
    realtest_mark("CORE_VMALLOC_OK");
    realtest_mark("CORE_TLB_BEGIN");
    if (!vmalloc_tlb_reuse_self_test()) {
        return core_fail(hooks, "OS_TLB_REUSE_FAIL\r\n");
    }
    hooks->write("OS_TLB_REUSE_OK\r\n");
    realtest_mark("CORE_TLB_OK");
    kernel_debug_stage_ready(OS_DEBUG_PHASE_SPEC_3);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_3,
                       OS_DEBUG_STEP_PROGRESS, 4U);
    return 1;
}
