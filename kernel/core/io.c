#include "io.h"

BOOLEAN liteos_io_manager_init(LITEOS_IO_MANAGER *manager) {
    if (manager == 0 || manager->Initialized ||
        !liteos_slab_cache_init(&manager->IrpCache, sizeof(LITEOS_IRP), 16U)) return 0;
    manager->ActiveIrpCount = 0;
    manager->Initialized = 1;
    return 1;
}

BOOLEAN liteos_io_manager_destroy(LITEOS_IO_MANAGER *manager) {
    if (manager == 0 || !manager->Initialized || manager->ActiveIrpCount != 0) return 0;
    liteos_slab_cache_destroy(&manager->IrpCache);
    manager->ActiveIrpCount = 0;
    manager->Initialized = 0;
    return 1;
}

LITEOS_IRP *liteos_irp_alloc(LITEOS_IO_MANAGER *manager, UINT32 operation,
                             VOID *buffer, UINT64 size,
                             LITEOS_IRP_COMPLETION completion, VOID *context) {
    if (manager == 0 || !manager->Initialized || operation == 0) return 0;
    LITEOS_IRP *irp = (LITEOS_IRP *)liteos_slab_alloc(&manager->IrpCache);
    if (irp == 0) return 0;
    irp->Operation = operation;
    irp->Flags = 0;
    irp->Buffer = buffer;
    irp->Size = size;
    irp->Status = 0;
    irp->Completion = completion;
    irp->Context = context;
    irp->Completed = 0;
    irp->Cancelled = 0;
    ++manager->ActiveIrpCount;
    return irp;
}

BOOLEAN liteos_irp_complete(LITEOS_IRP *irp, INT64 status) {
    if (irp == 0 || irp->Completed) return 0;
    irp->Status = status;
    irp->Completed = 1;
    if (irp->Completion != 0) irp->Completion(irp, irp->Context);
    return 1;
}

BOOLEAN liteos_irp_cancel(LITEOS_IRP *irp) {
    if (irp == 0 || irp->Completed) return 0;
    irp->Cancelled = 1;
    return liteos_irp_complete(irp, -1);
}

VOID liteos_irp_free(LITEOS_IO_MANAGER *manager, LITEOS_IRP *irp) {
    if (manager == 0 || !manager->Initialized || irp == 0 || !irp->Completed) return;
    if (liteos_slab_free(&manager->IrpCache, irp) && manager->ActiveIrpCount != 0) {
        --manager->ActiveIrpCount;
    }
}
