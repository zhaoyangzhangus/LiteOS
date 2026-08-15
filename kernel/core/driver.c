#include "driver.h"

static BOOLEAN copy_name(CHAR8 *destination, const CHAR8 *source) {
    if (destination == 0 || source == 0) return 0;
    for (UINT32 i = 0; i + 1U < LITEOS_DRIVER_NAME_LENGTH; ++i) {
        destination[i] = source[i];
        if (source[i] == 0) return 1;
    }
    destination[LITEOS_DRIVER_NAME_LENGTH - 1U] = 0;
    return source[LITEOS_DRIVER_NAME_LENGTH - 1U] == 0;
}

static LITEOS_DRIVER *find_driver(LITEOS_DRIVER_MANAGER *manager,
                                  const LITEOS_DRIVER *driver) {
    if (manager == 0 || driver == 0) return 0;
    for (UINT32 i = 0; i < LITEOS_DRIVER_TABLE_SIZE; ++i) {
        if (&manager->Drivers[i] == driver && manager->Drivers[i].Registered) {
            return &manager->Drivers[i];
        }
    }
    return 0;
}

static LITEOS_DEVICE *find_device(LITEOS_DRIVER_MANAGER *manager,
                                  const LITEOS_DEVICE *device) {
    if (manager == 0 || device == 0) return 0;
    for (UINT32 i = 0; i < LITEOS_DEVICE_TABLE_SIZE; ++i) {
        if (&manager->Devices[i] == device && manager->Devices[i].Registered) {
            return &manager->Devices[i];
        }
    }
    return 0;
}

static UINT32 irp_access(const LITEOS_IRP *irp) {
    if (irp == 0) return 0;
    switch (irp->Operation) {
        case LITEOS_IRP_READ:
            return LITEOS_ACCESS_READ;
        case LITEOS_IRP_WRITE:
        case LITEOS_IRP_FLUSH:
            return LITEOS_ACCESS_WRITE;
        case LITEOS_IRP_DEVICE_CONTROL:
            return LITEOS_ACCESS_ADMIN;
        default:
            return 0;
    }
}

static BOOLEAN device_access_allowed(const LITEOS_DRIVER_MANAGER *manager,
                                     const LITEOS_DEVICE *device,
                                     const LITEOS_IRP *irp) {
    UINT32 desired_access;
    if (device->SecurityDescriptor == 0) return 1;
    desired_access = irp_access(irp);
    return desired_access != 0U && manager->HasSecurityToken &&
           liteos_security_access_check(&manager->SecurityToken,
                                        device->SecurityDescriptor,
                                        desired_access);
}

BOOLEAN liteos_driver_manager_init(LITEOS_DRIVER_MANAGER *manager) {
    if (manager == 0 || manager->Initialized) return 0;
    for (UINT32 i = 0; i < LITEOS_DRIVER_TABLE_SIZE; ++i) {
        manager->Drivers[i].Registered = 0;
        manager->Drivers[i].Identifier = 0;
        manager->Drivers[i].Name[0] = 0;
        manager->Drivers[i].Dispatch = 0;
        manager->Drivers[i].Context = 0;
        manager->Drivers[i].DeviceCount = 0;
    }
    for (UINT32 i = 0; i < LITEOS_DEVICE_TABLE_SIZE; ++i) {
        manager->Devices[i].Registered = 0;
        manager->Devices[i].Identifier = 0;
        manager->Devices[i].Name[0] = 0;
        manager->Devices[i].Driver = 0;
        manager->Devices[i].Context = 0;
        manager->Devices[i].SecurityDescriptor = 0;
    }
    manager->DriverCount = 0;
    manager->DeviceCount = 0;
    manager->SecurityToken.UserId = 0;
    manager->SecurityToken.GroupId = 0;
    manager->SecurityToken.Capabilities = 0;
    manager->HasSecurityToken = 0;
    manager->Initialized = 1;
    return 1;
}

BOOLEAN liteos_driver_manager_destroy(LITEOS_DRIVER_MANAGER *manager) {
    if (manager == 0 || !manager->Initialized || manager->DriverCount != 0 ||
        manager->DeviceCount != 0) return 0;
    manager->Initialized = 0;
    return 1;
}

LITEOS_DRIVER *liteos_driver_register(LITEOS_DRIVER_MANAGER *manager,
                                      const CHAR8 *name,
                                      LITEOS_DRIVER_DISPATCH dispatch,
                                      VOID *context) {
    if (manager == 0 || !manager->Initialized || name == 0 || dispatch == 0) return 0;
    for (UINT32 i = 0; i < LITEOS_DRIVER_TABLE_SIZE; ++i) {
        LITEOS_DRIVER *driver = &manager->Drivers[i];
        if (driver->Registered || !copy_name(driver->Name, name)) continue;
        driver->Registered = 1;
        driver->Identifier = i + 1U;
        driver->Dispatch = dispatch;
        driver->Context = context;
        driver->DeviceCount = 0;
        ++manager->DriverCount;
        return driver;
    }
    return 0;
}

BOOLEAN liteos_driver_unregister(LITEOS_DRIVER_MANAGER *manager,
                                 LITEOS_DRIVER *driver) {
    driver = find_driver(manager, driver);
    if (driver == 0 || driver->DeviceCount != 0) return 0;
    driver->Registered = 0;
    driver->Identifier = 0;
    driver->Name[0] = 0;
    driver->Dispatch = 0;
    driver->Context = 0;
    if (manager->DriverCount != 0) --manager->DriverCount;
    return 1;
}

LITEOS_DEVICE *liteos_device_register(LITEOS_DRIVER_MANAGER *manager,
                                      const CHAR8 *name,
                                      LITEOS_DRIVER *driver,
                                      VOID *context) {
    if (manager == 0 || !manager->Initialized || name == 0) return 0;
    driver = find_driver(manager, driver);
    if (driver == 0) return 0;
    for (UINT32 i = 0; i < LITEOS_DEVICE_TABLE_SIZE; ++i) {
        LITEOS_DEVICE *device = &manager->Devices[i];
        if (device->Registered || !copy_name(device->Name, name)) continue;
        device->Registered = 1;
        device->Identifier = i + 1U;
        device->Driver = driver;
        device->Context = context;
        device->SecurityDescriptor = 0;
        ++driver->DeviceCount;
        ++manager->DeviceCount;
        return device;
    }
    return 0;
}

BOOLEAN liteos_device_unregister(LITEOS_DRIVER_MANAGER *manager,
                                 LITEOS_DEVICE *device) {
    device = find_device(manager, device);
    if (device == 0 || find_driver(manager, device->Driver) == 0) return 0;
    if (device->Driver->DeviceCount != 0) --device->Driver->DeviceCount;
    device->Registered = 0;
    device->Identifier = 0;
    device->Name[0] = 0;
    device->Driver = 0;
    device->Context = 0;
    if (manager->DeviceCount != 0) --manager->DeviceCount;
    return 1;
}

BOOLEAN liteos_device_set_security_descriptor(
    LITEOS_DRIVER_MANAGER *manager, LITEOS_DEVICE *device,
    LITEOS_SECURITY_DESCRIPTOR *descriptor) {
    device = find_device(manager, device);
    if (device == 0) return 0;
    device->SecurityDescriptor = descriptor;
    return 1;
}

BOOLEAN liteos_driver_manager_set_token(
    LITEOS_DRIVER_MANAGER *manager, const LITEOS_SECURITY_TOKEN *token) {
    if (manager == 0 || !manager->Initialized) return 0;
    if (token == 0) {
        manager->HasSecurityToken = 0;
        return 1;
    }
    manager->SecurityToken = *token;
    manager->HasSecurityToken = 1;
    return 1;
}

BOOLEAN liteos_driver_submit(LITEOS_DRIVER_MANAGER *manager,
                             LITEOS_DEVICE *device, LITEOS_IRP *irp) {
    device = find_device(manager, device);
    if (device == 0 || irp == 0 || irp->Completed || device->Driver == 0 ||
        device->Driver->Dispatch == 0 || !device_access_allowed(manager, device, irp)) return 0;
    return device->Driver->Dispatch(device, irp);
}
