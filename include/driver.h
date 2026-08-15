#ifndef LITEOS_DRIVER_H
#define LITEOS_DRIVER_H

#include "io.h"
#include "security.h"

/* 启动阶段使用固定表，避免在驱动管理器尚未稳定时依赖动态内存。 */
#define LITEOS_DRIVER_NAME_LENGTH 32U
#define LITEOS_DRIVER_TABLE_SIZE  32U
#define LITEOS_DEVICE_TABLE_SIZE 64U

typedef UINT32 LITEOS_DRIVER_ID;
typedef UINT32 LITEOS_DEVICE_ID;

typedef struct LITEOS_DEVICE LITEOS_DEVICE;
typedef struct LITEOS_DRIVER LITEOS_DRIVER;

/* 驱动返回 TRUE 表示已接受请求；请求可以同步完成，也可以异步完成。 */
typedef BOOLEAN (*LITEOS_DRIVER_DISPATCH)(LITEOS_DEVICE *device, LITEOS_IRP *irp);

struct LITEOS_DRIVER {
    BOOLEAN Registered;
    LITEOS_DRIVER_ID Identifier;
    CHAR8 Name[LITEOS_DRIVER_NAME_LENGTH];
    LITEOS_DRIVER_DISPATCH Dispatch;
    VOID *Context;
    UINT32 DeviceCount;
};

struct LITEOS_DEVICE {
    BOOLEAN Registered;
    LITEOS_DEVICE_ID Identifier;
    CHAR8 Name[LITEOS_DRIVER_NAME_LENGTH];
    LITEOS_DRIVER *Driver;
    VOID *Context;
    LITEOS_SECURITY_DESCRIPTOR *SecurityDescriptor;
};

typedef struct {
    LITEOS_DRIVER Drivers[LITEOS_DRIVER_TABLE_SIZE];
    LITEOS_DEVICE Devices[LITEOS_DEVICE_TABLE_SIZE];
    UINT32 DriverCount;
    UINT32 DeviceCount;
    LITEOS_SECURITY_TOKEN SecurityToken;
    BOOLEAN HasSecurityToken;
    BOOLEAN Initialized;
} LITEOS_DRIVER_MANAGER;

BOOLEAN liteos_driver_manager_init(LITEOS_DRIVER_MANAGER *manager);
BOOLEAN liteos_driver_manager_destroy(LITEOS_DRIVER_MANAGER *manager);

LITEOS_DRIVER *liteos_driver_register(LITEOS_DRIVER_MANAGER *manager,
                                      const CHAR8 *name,
                                      LITEOS_DRIVER_DISPATCH dispatch,
                                      VOID *context);
BOOLEAN liteos_driver_unregister(LITEOS_DRIVER_MANAGER *manager,
                                 LITEOS_DRIVER *driver);

LITEOS_DEVICE *liteos_device_register(LITEOS_DRIVER_MANAGER *manager,
                                      const CHAR8 *name,
                                      LITEOS_DRIVER *driver,
                                      VOID *context);
BOOLEAN liteos_device_unregister(LITEOS_DRIVER_MANAGER *manager,
                                 LITEOS_DEVICE *device);
BOOLEAN liteos_device_set_security_descriptor(
    LITEOS_DRIVER_MANAGER *manager, LITEOS_DEVICE *device,
    LITEOS_SECURITY_DESCRIPTOR *descriptor);
BOOLEAN liteos_driver_manager_set_token(
    LITEOS_DRIVER_MANAGER *manager, const LITEOS_SECURITY_TOKEN *token);

BOOLEAN liteos_driver_submit(LITEOS_DRIVER_MANAGER *manager,
                             LITEOS_DEVICE *device, LITEOS_IRP *irp);

#endif
