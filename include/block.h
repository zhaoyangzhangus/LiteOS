#ifndef LITEOS_BLOCK_H
#define LITEOS_BLOCK_H

#include "uefi.h"

#define LITEOS_BLOCK_DEVICE_COUNT 8U
#define LITEOS_BLOCK_NAME_LENGTH  32U

typedef BOOLEAN (*LITEOS_BLOCK_READ)(VOID *context, UINT64 lba,
                                    UINT32 count, VOID *buffer);
typedef BOOLEAN (*LITEOS_BLOCK_WRITE)(VOID *context, UINT64 lba,
                                     UINT32 count, const VOID *buffer);
typedef BOOLEAN (*LITEOS_BLOCK_FLUSH)(VOID *context);

typedef struct {
    CHAR8 Name[LITEOS_BLOCK_NAME_LENGTH];
    UINT32 BlockSize;
    UINT64 BlockCount;
    LITEOS_BLOCK_READ Read;
    LITEOS_BLOCK_WRITE Write;
    LITEOS_BLOCK_FLUSH Flush;
    VOID *Context;
    BOOLEAN Registered;
} LITEOS_BLOCK_DEVICE;

typedef struct {
    LITEOS_BLOCK_DEVICE Devices[LITEOS_BLOCK_DEVICE_COUNT];
    UINT32 DeviceCount;
    BOOLEAN Initialized;
} LITEOS_BLOCK_MANAGER;

BOOLEAN liteos_block_manager_init(LITEOS_BLOCK_MANAGER *manager);
BOOLEAN liteos_block_manager_destroy(LITEOS_BLOCK_MANAGER *manager);
BOOLEAN liteos_block_register(LITEOS_BLOCK_MANAGER *manager,
                              const CHAR8 *name, UINT32 block_size,
                              UINT64 block_count, LITEOS_BLOCK_READ read,
                              LITEOS_BLOCK_WRITE write,
                              LITEOS_BLOCK_FLUSH flush, VOID *context,
                              LITEOS_BLOCK_DEVICE **device);
BOOLEAN liteos_block_unregister(LITEOS_BLOCK_MANAGER *manager,
                                LITEOS_BLOCK_DEVICE *device);
BOOLEAN liteos_block_read(const LITEOS_BLOCK_DEVICE *device, UINT64 lba,
                          UINT32 count, VOID *buffer);
BOOLEAN liteos_block_write(const LITEOS_BLOCK_DEVICE *device, UINT64 lba,
                           UINT32 count, const VOID *buffer);
BOOLEAN liteos_block_flush(const LITEOS_BLOCK_DEVICE *device);

#endif
