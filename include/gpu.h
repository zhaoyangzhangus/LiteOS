#ifndef LITEOS_GPU_H
#define LITEOS_GPU_H

#include "buddy.h"

#define LITEOS_GPU_CONTEXT_COUNT    16U
#define LITEOS_GPU_ALLOCATION_COUNT 32U
#define LITEOS_GPU_COMMAND_COUNT    32U

typedef struct {
    BOOLEAN Active;
    UINT32 Identifier;
    UINT32 OwnerProcess;
} LITEOS_GPU_CONTEXT;

typedef struct {
    BOOLEAN Active;
    UINT32 Identifier;
    UINT64 Size;
    LITEOS_PHYSICAL_BLOCK PhysicalBlock;
} LITEOS_GPU_ALLOCATION;

typedef struct {
    BOOLEAN Active;
    UINT32 Identifier;
    UINT32 ContextIdentifier;
    UINT32 AllocationIdentifier;
    UINT64 CommandSize;
} LITEOS_GPU_COMMAND_BUFFER;

typedef struct {
    UINT64 Value;
    BOOLEAN Signaled;
} LITEOS_GPU_FENCE;

typedef struct {
    LITEOS_GPU_CONTEXT Contexts[LITEOS_GPU_CONTEXT_COUNT];
    LITEOS_GPU_ALLOCATION Allocations[LITEOS_GPU_ALLOCATION_COUNT];
    LITEOS_GPU_COMMAND_BUFFER Commands[LITEOS_GPU_COMMAND_COUNT];
    UINT32 ContextCount;
    UINT32 AllocationCount;
    UINT32 CommandCount;
    UINT64 NextFenceValue;
    BOOLEAN Initialized;
} LITEOS_GPU_MANAGER;

BOOLEAN liteos_gpu_manager_init(LITEOS_GPU_MANAGER *manager);
BOOLEAN liteos_gpu_manager_destroy(LITEOS_GPU_MANAGER *manager);
LITEOS_GPU_CONTEXT *liteos_gpu_context_create(LITEOS_GPU_MANAGER *manager,
                                               UINT32 owner_process);
BOOLEAN liteos_gpu_context_destroy(LITEOS_GPU_MANAGER *manager,
                                   LITEOS_GPU_CONTEXT *context);
LITEOS_GPU_ALLOCATION *liteos_gpu_allocation_create(LITEOS_GPU_MANAGER *manager,
                                                    UINT64 size);
BOOLEAN liteos_gpu_allocation_destroy(LITEOS_GPU_MANAGER *manager,
                                      LITEOS_GPU_ALLOCATION *allocation);
LITEOS_GPU_COMMAND_BUFFER *liteos_gpu_command_create(LITEOS_GPU_MANAGER *manager,
                                                     LITEOS_GPU_CONTEXT *context,
                                                     LITEOS_GPU_ALLOCATION *allocation,
                                                     UINT64 command_size);
BOOLEAN liteos_gpu_submit(LITEOS_GPU_MANAGER *manager,
                          LITEOS_GPU_COMMAND_BUFFER *command,
                          LITEOS_GPU_FENCE *fence);
BOOLEAN liteos_gpu_fence_wait(const LITEOS_GPU_FENCE *fence);

#endif
