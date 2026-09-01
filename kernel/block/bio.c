#include <kernel/block_device.h>

static BOOLEAN copy_name(CHAR8 *destination, const CHAR8 *source) {
    UINT32 i;
    if (destination == 0 || source == 0) return 0;
    for (i = 0; i + 1U < LITEOS_BLOCK_NAME_LENGTH; ++i) {
        destination[i] = source[i];
        if (source[i] == 0) return 1;
    }
    destination[LITEOS_BLOCK_NAME_LENGTH - 1U] = 0;
    return source[i] == 0;
}

BOOLEAN liteos_block_manager_init(LITEOS_BLOCK_MANAGER *manager) {
    if (manager == 0 || manager->Initialized) return 0;
    for (UINT32 i = 0; i < LITEOS_BLOCK_DEVICE_COUNT; ++i) {
        manager->Devices[i].Registered = 0;
        manager->Devices[i].Name[0] = 0;
        manager->Devices[i].Read = 0;
        manager->Devices[i].Write = 0;
        manager->Devices[i].Flush = 0;
        manager->Devices[i].Context = 0;
    }
    manager->DeviceCount = 0;
    manager->Initialized = 1;
    return 1;
}

BOOLEAN liteos_block_manager_destroy(LITEOS_BLOCK_MANAGER *manager) {
    if (manager == 0 || !manager->Initialized || manager->DeviceCount != 0U) return 0;
    manager->Initialized = 0;
    return 1;
}

BOOLEAN liteos_block_register(LITEOS_BLOCK_MANAGER *manager,
                              const CHAR8 *name, UINT32 block_size,
                              UINT64 block_count, LITEOS_BLOCK_READ read,
                              LITEOS_BLOCK_WRITE write,
                              LITEOS_BLOCK_FLUSH flush, VOID *context,
                              LITEOS_BLOCK_DEVICE **device) {
    if (manager == 0 || !manager->Initialized || name == 0 || block_size == 0U ||
        block_count == 0 || read == 0 || device == 0) return 0;
    for (UINT32 i = 0; i < LITEOS_BLOCK_DEVICE_COUNT; ++i) {
        LITEOS_BLOCK_DEVICE *candidate = &manager->Devices[i];
        if (candidate->Registered || !copy_name(candidate->Name, name)) continue;
        candidate->BlockSize = block_size;
        candidate->BlockCount = block_count;
        candidate->Read = read;
        candidate->Write = write;
        candidate->Flush = flush;
        candidate->Context = context;
        candidate->Registered = 1;
        ++manager->DeviceCount;
        *device = candidate;
        return 1;
    }
    return 0;
}

BOOLEAN liteos_block_unregister(LITEOS_BLOCK_MANAGER *manager,
                                LITEOS_BLOCK_DEVICE *device) {
    if (manager == 0 || !manager->Initialized || device == 0 || !device->Registered) return 0;
    BOOLEAN belongs = 0;
    for (UINT32 i = 0; i < LITEOS_BLOCK_DEVICE_COUNT; ++i) {
        if (&manager->Devices[i] == device) belongs = 1;
    }
    if (!belongs) return 0;
    device->Registered = 0;
    device->Name[0] = 0;
    device->Read = 0;
    device->Write = 0;
    device->Flush = 0;
    device->Context = 0;
    if (manager->DeviceCount != 0U) --manager->DeviceCount;
    return 1;
}

BOOLEAN liteos_block_read(const LITEOS_BLOCK_DEVICE *device, UINT64 lba,
                          UINT32 count, VOID *buffer) {
    if (device == 0 || !device->Registered || device->Read == 0 || buffer == 0 ||
        count == 0U || lba >= device->BlockCount || count > device->BlockCount - lba) return 0;
    return device->Read(device->Context, lba, count, buffer);
}

BOOLEAN liteos_block_write(const LITEOS_BLOCK_DEVICE *device, UINT64 lba,
                           UINT32 count, const VOID *buffer) {
    if (device == 0 || !device->Registered || device->Write == 0 || buffer == 0 ||
        count == 0U || lba >= device->BlockCount || count > device->BlockCount - lba) return 0;
    return device->Write(device->Context, lba, count, buffer);
}

BOOLEAN liteos_block_flush(const LITEOS_BLOCK_DEVICE *device) {
    if (device == 0 || !device->Registered) return 0;
    return device->Flush == 0 || device->Flush(device->Context);
}
