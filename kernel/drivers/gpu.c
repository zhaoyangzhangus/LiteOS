#include "gpu.h"

static BOOLEAN context_belongs(const LITEOS_GPU_MANAGER *manager,
                               const LITEOS_GPU_CONTEXT *context) {
    if (manager == 0 || context == 0) return 0;
    for (UINT32 i = 0; i < LITEOS_GPU_CONTEXT_COUNT; ++i) {
        if (&manager->Contexts[i] == context && context->Active) return 1;
    }
    return 0;
}

static BOOLEAN allocation_belongs(const LITEOS_GPU_MANAGER *manager,
                                  const LITEOS_GPU_ALLOCATION *allocation) {
    if (manager == 0 || allocation == 0) return 0;
    for (UINT32 i = 0; i < LITEOS_GPU_ALLOCATION_COUNT; ++i) {
        if (&manager->Allocations[i] == allocation && allocation->Active) return 1;
    }
    return 0;
}

static BOOLEAN command_belongs(const LITEOS_GPU_MANAGER *manager,
                               const LITEOS_GPU_COMMAND_BUFFER *command) {
    if (manager == 0 || command == 0) return 0;
    for (UINT32 i = 0; i < LITEOS_GPU_COMMAND_COUNT; ++i) {
        if (&manager->Commands[i] == command && command->Active) return 1;
    }
    return 0;
}

BOOLEAN liteos_gpu_manager_init(LITEOS_GPU_MANAGER *manager) {
    if (manager == 0 || manager->Initialized) return 0;
    for (UINT32 i = 0; i < LITEOS_GPU_CONTEXT_COUNT; ++i) manager->Contexts[i].Active = 0;
    for (UINT32 i = 0; i < LITEOS_GPU_ALLOCATION_COUNT; ++i) manager->Allocations[i].Active = 0;
    for (UINT32 i = 0; i < LITEOS_GPU_COMMAND_COUNT; ++i) manager->Commands[i].Active = 0;
    manager->ContextCount = 0;
    manager->AllocationCount = 0;
    manager->CommandCount = 0;
    manager->NextFenceValue = 0;
    manager->Initialized = 1;
    return 1;
}

BOOLEAN liteos_gpu_manager_destroy(LITEOS_GPU_MANAGER *manager) {
    if (manager == 0 || !manager->Initialized || manager->ContextCount != 0 ||
        manager->CommandCount != 0) return 0;
    for (UINT32 i = 0; i < LITEOS_GPU_ALLOCATION_COUNT; ++i) {
        if (manager->Allocations[i].Active &&
            !liteos_gpu_allocation_destroy(manager, &manager->Allocations[i])) return 0;
    }
    manager->Initialized = 0;
    return 1;
}

LITEOS_GPU_CONTEXT *liteos_gpu_context_create(LITEOS_GPU_MANAGER *manager,
                                               UINT32 owner_process) {
    if (manager == 0 || !manager->Initialized) return 0;
    for (UINT32 i = 0; i < LITEOS_GPU_CONTEXT_COUNT; ++i) {
        LITEOS_GPU_CONTEXT *context = &manager->Contexts[i];
        if (context->Active) continue;
        context->Active = 1;
        context->Identifier = i + 1U;
        context->OwnerProcess = owner_process;
        ++manager->ContextCount;
        return context;
    }
    return 0;
}

BOOLEAN liteos_gpu_context_destroy(LITEOS_GPU_MANAGER *manager,
                                   LITEOS_GPU_CONTEXT *context) {
    if (manager == 0 || !manager->Initialized || !context_belongs(manager, context)) return 0;
    for (UINT32 i = 0; i < LITEOS_GPU_COMMAND_COUNT; ++i) {
        if (manager->Commands[i].Active &&
            manager->Commands[i].ContextIdentifier == context->Identifier) return 0;
    }
    context->Active = 0;
    if (manager->ContextCount != 0) --manager->ContextCount;
    return 1;
}

LITEOS_GPU_ALLOCATION *liteos_gpu_allocation_create(LITEOS_GPU_MANAGER *manager,
                                                    UINT64 size) {
    if (manager == 0 || !manager->Initialized || size == 0) return 0;
    for (UINT32 i = 0; i < LITEOS_GPU_ALLOCATION_COUNT; ++i) {
        LITEOS_GPU_ALLOCATION *allocation = &manager->Allocations[i];
        if (allocation->Active || !liteos_buddy_alloc_bytes(size, &allocation->PhysicalBlock)) continue;
        allocation->Active = 1;
        allocation->Identifier = i + 1U;
        allocation->Size = size;
        ++manager->AllocationCount;
        return allocation;
    }
    return 0;
}

BOOLEAN liteos_gpu_allocation_destroy(LITEOS_GPU_MANAGER *manager,
                                      LITEOS_GPU_ALLOCATION *allocation) {
    if (manager == 0 || !manager->Initialized || !allocation_belongs(manager, allocation)) return 0;
    for (UINT32 i = 0; i < LITEOS_GPU_COMMAND_COUNT; ++i) {
        if (manager->Commands[i].Active &&
            manager->Commands[i].AllocationIdentifier == allocation->Identifier) return 0;
    }
    if (!liteos_buddy_free(&allocation->PhysicalBlock)) return 0;
    allocation->Active = 0;
    allocation->Identifier = 0;
    allocation->Size = 0;
    if (manager->AllocationCount != 0) --manager->AllocationCount;
    return 1;
}

LITEOS_GPU_COMMAND_BUFFER *liteos_gpu_command_create(LITEOS_GPU_MANAGER *manager,
                                                     LITEOS_GPU_CONTEXT *context,
                                                     LITEOS_GPU_ALLOCATION *allocation,
                                                     UINT64 command_size) {
    if (manager == 0 || !manager->Initialized || !context_belongs(manager, context) ||
        !allocation_belongs(manager, allocation) || command_size == 0 ||
        command_size > allocation->Size) return 0;
    for (UINT32 i = 0; i < LITEOS_GPU_COMMAND_COUNT; ++i) {
        LITEOS_GPU_COMMAND_BUFFER *command = &manager->Commands[i];
        if (command->Active) continue;
        command->Active = 1;
        command->Identifier = i + 1U;
        command->ContextIdentifier = context->Identifier;
        command->AllocationIdentifier = allocation->Identifier;
        command->CommandSize = command_size;
        ++manager->CommandCount;
        return command;
    }
    return 0;
}

BOOLEAN liteos_gpu_submit(LITEOS_GPU_MANAGER *manager,
                          LITEOS_GPU_COMMAND_BUFFER *command,
                          LITEOS_GPU_FENCE *fence) {
    if (manager == 0 || !manager->Initialized || fence == 0 ||
        !command_belongs(manager, command)) return 0;
    fence->Value = ++manager->NextFenceValue;
    fence->Signaled = 1;
    /* 当前是软件完成路径，真实 GPU 驱动会在硬件完成中断中置位 fence。 */
    command->Active = 0;
    if (manager->CommandCount != 0) --manager->CommandCount;
    return 1;
}

BOOLEAN liteos_gpu_fence_wait(const LITEOS_GPU_FENCE *fence) {
    return fence != 0 && fence->Signaled;
}
