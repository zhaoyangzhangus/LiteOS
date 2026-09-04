#include <kernel/dma.h>
#include <kernel/firmware.h>
#include <kernel/mm.h>

#include <stdio.h>
#include <threads.h>

#include "../../kernel/drivers/gpu/nvidia/port/include/nv_port.h"

#define NV_PORT_SMP_THREADS 4U
#define NV_PORT_SMP_OPERATIONS 25000U
#define NV_PORT_VM_LEAK_ROUNDS 2048U

typedef struct nv_port_smp_context {
    nv_port_atomic32_t *counter;
    uint32_t operations;
} nv_port_smp_context_t;

static int nv_port_smp_worker(void *argument) {
    nv_port_smp_context_t *context = (nv_port_smp_context_t *)argument;
    if (context == 0 || context->counter == 0) return 1;
    for (uint32_t index = 0U; index < context->operations; ++index) {
        (void)nv_port_atomic32_fetch_add(context->counter, 1U);
    }
    return 0;
}

static bool nv_port_smp_stress(void) {
    nv_port_atomic32_t counter;
    nv_port_smp_context_t context;
    thrd_t threads[NV_PORT_SMP_THREADS];
    uint32_t created = 0U;

    nv_port_atomic32_init(&counter, 0U);
    context.counter = &counter;
    context.operations = NV_PORT_SMP_OPERATIONS;
    for (; created < NV_PORT_SMP_THREADS; ++created) {
        if (thrd_create(&threads[created], nv_port_smp_worker, &context) !=
            thrd_success) {
            for (uint32_t index = 0U; index < created; ++index) {
                (void)thrd_join(threads[index], 0);
            }
            return false;
        }
    }
    bool success = true;
    for (uint32_t index = 0U; index < created; ++index) {
        int result = 1;
        if (thrd_join(threads[index], &result) != thrd_success || result != 0) {
            success = false;
        }
    }
    return success && nv_port_atomic32_load(&counter) ==
           NV_PORT_SMP_THREADS * NV_PORT_SMP_OPERATIONS;
}

static bool nv_port_vm_map_unmap_leak_test(void) {
    for (uint32_t index = 0U; index < NV_PORT_VM_LEAK_ROUNDS; ++index) {
        nv_port_vm_mapping_t mapping = {0};
        if (nv_port_vm_map_device(
                0, paddr_make(0x100000U + (uint64_t)index * PAGE_SIZE),
                PAGE_SIZE, 0U, 0U, 0U, NV_PORT_CACHE_UC, &mapping) != K_OK ||
            !mapping.active || nv_port_vm_unmap(&mapping) != K_OK ||
            mapping.active) return false;
    }
    return true;
}

static bool nv_port_dma_descriptor_test(void) {
    const uint64_t pages[] = {0x1000U, 0x2000U, 0x5000U, 0x6000U,
                              0x7000U, 0xB000U};
    nv_port_dma_segment_t segments[6] = {0};
    uint32_t count = 0U;
    if (nv_port_dma_build_segments(pages, 6U, segments, 6U, &count) != K_OK ||
        count != 3U || segments[0].length != 2U * PAGE_SIZE ||
        segments[1].phys_addr != 0x5000U ||
        segments[1].length != 3U * PAGE_SIZE ||
        segments[2].phys_addr != 0xB000U ||
        segments[2].length != PAGE_SIZE) return false;
    return nv_port_dma_build_segments(pages, 6U, segments, 2U, &count) ==
           K_ENOSPC;
}

int main(void) {
    bool ports = true;
    bool result;
#define NV_PORT_CHECK(name)                                                   \
    do {                                                                      \
        result = (name)();                                                    \
        if (!result) {                                                        \
            printf("PORT_SELF_TEST_FAIL=%s\n", #name);                      \
            ports = false;                                                    \
        }                                                                     \
    } while (0)
    NV_PORT_CHECK(nv_port_memory_self_test);
    NV_PORT_CHECK(nv_port_atomic_self_test);
    NV_PORT_CHECK(nv_port_cache_self_test);
    NV_PORT_CHECK(nv_port_sync_self_test);
    NV_PORT_CHECK(nv_port_time_self_test);
    NV_PORT_CHECK(nv_port_thread_self_test);
    NV_PORT_CHECK(nv_port_worker_self_test);
    NV_PORT_CHECK(nv_port_vm_self_test);
    NV_PORT_CHECK(nv_port_pci_self_test);
    NV_PORT_CHECK(nv_port_mmio_self_test);
    NV_PORT_CHECK(nv_port_vbios_self_test);
    NV_PORT_CHECK(nv_port_dma_self_test);
    NV_PORT_CHECK(nv_port_irq_self_test);
    NV_PORT_CHECK(nv_port_firmware_self_test);
    NV_PORT_CHECK(nv_port_registry_self_test);
#undef NV_PORT_CHECK
    bool smp = nv_port_smp_stress();
    bool vm_leak = nv_port_vm_map_unmap_leak_test();
    bool dma = nv_port_dma_descriptor_test();
    bool irq = nv_port_irq_vector_stress_self_test();

    if (!ports || !smp || !vm_leak || !dma || !irq) {
        puts("PORT_TESTS=FAIL");
        return 1;
    }
    puts("PORT_TESTS=PASS");
    puts("SMP_STRESS=PASS");
    puts("VM_MAP_UNMAP_LEAK=PASS");
    puts("DMA_DESCRIPTOR=PASS");
    puts("IRQ_VECTOR_STRESS=PASS");
    return 0;
}
