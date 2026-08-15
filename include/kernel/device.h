#pragma once
#include "../../OS_Implementation_Specification_COMPLETE/include/kernel/device.h"
#include "io.h"

/* 设备移除与用户取消共享同一 CAS 状态机，但保留各自的完成错误码。 */

#ifndef KOBJECT_TYPE_DEVICE
#define KOBJECT_TYPE_DEVICE 0x010BU
#endif

/* 规范结构没有隐含构造函数，内核内部用这两个助手初始化裸设备对象。 */
void device_object_init(device_t *dev, uint64_t device_id, uint32_t class_id,
                        const device_ops_t *ops, void *bus_data);
void driver_object_init(driver_t *drv, const char *name,
                        kstatus_t (*probe)(device_t *),
                        void (*remove)(device_t *));
