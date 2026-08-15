#ifndef LITEOS_IO_H
#define LITEOS_IO_H

#include "slab.h"

enum {
    LITEOS_IRP_READ = 1U,
    LITEOS_IRP_WRITE,
    LITEOS_IRP_DEVICE_CONTROL,
    LITEOS_IRP_FLUSH,
};

typedef struct LITEOS_IRP LITEOS_IRP;
typedef VOID (*LITEOS_IRP_COMPLETION)(LITEOS_IRP *irp, VOID *context);

struct LITEOS_IRP {
    UINT32 Operation;
    UINT32 Flags;
    VOID *Buffer;
    UINT64 Size;
    INT64 Status;
    LITEOS_IRP_COMPLETION Completion;
    VOID *Context;
    BOOLEAN Completed;
    BOOLEAN Cancelled;
};

typedef struct {
    LITEOS_SLAB_CACHE IrpCache;
    UINT32 ActiveIrpCount;
    BOOLEAN Initialized;
} LITEOS_IO_MANAGER;

BOOLEAN liteos_io_manager_init(LITEOS_IO_MANAGER *manager);
BOOLEAN liteos_io_manager_destroy(LITEOS_IO_MANAGER *manager);
LITEOS_IRP *liteos_irp_alloc(LITEOS_IO_MANAGER *manager, UINT32 operation,
                             VOID *buffer, UINT64 size,
                             LITEOS_IRP_COMPLETION completion, VOID *context);
BOOLEAN liteos_irp_complete(LITEOS_IRP *irp, INT64 status);
BOOLEAN liteos_irp_cancel(LITEOS_IRP *irp);
VOID liteos_irp_free(LITEOS_IO_MANAGER *manager, LITEOS_IRP *irp);

#endif
