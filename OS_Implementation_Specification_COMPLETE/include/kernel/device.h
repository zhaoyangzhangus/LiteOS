#pragma once
#include "base.h"
#include "object.h"
#include "list.h"
#include "spinlock.h"
#include "io.h"

struct bus_type;
struct driver;
struct module;
struct power_device;

#define KOBJECT_TYPE_DEVICE 0x010BU

#define LITEOS_DRIVER_API_VERSION 1U

enum device_state {
    DEVICE_DISCOVERED = 0,
    DEVICE_ENUMERATED,
    DEVICE_BOUND,
    DEVICE_ACTIVE,
    DEVICE_SUSPENDED,
    DEVICE_RECOVERING,
    DEVICE_REMOVING,
    DEVICE_REMOVED,
    DEVICE_FAILED,
};

/* set_power 的统一电源状态参数。 */
#define DEVICE_POWER_ACTIVE   0U
#define DEVICE_POWER_SUSPENDED 1U
#define DEVICE_DEFAULT_POWER_TIMEOUT_NS 100000000ULL

typedef struct resource {
    uint32_t type;
    uint32_t flags;
    uint64_t start;
    uint64_t length;
} resource_t;

typedef struct device_ops {
    kstatus_t (*start)(struct device *);
    void (*stop)(struct device *);
    kstatus_t (*submit_io)(struct device *, io_request_t *);
    kstatus_t (*reset)(struct device *, uint32_t level);
    kstatus_t (*set_power)(struct device *, uint32_t state);
    /* 可选的非阻塞电源事务；K_EAGAIN 表示仍在进行。 */
    kstatus_t (*begin_power)(struct device *, uint32_t state);
    kstatus_t (*poll_power)(struct device *, uint32_t state);
} device_ops_t;

typedef struct device {
    object_header_t object;
    uint64_t device_id;
    atomic_uint state;
    uint32_t class_id;

    struct device *parent;
    list_head_t sibling_node;
    list_head_t children;

    struct bus_type *bus;
    struct driver *driver;

    const device_ops_t *ops;
    void *bus_data;
    void *driver_data;

    resource_t *resources;
    uint32_t resource_count;

    spinlock_t state_lock;
    struct power_device *power_device;
    uint64_t power_timeout_ns;
    /* 已提交但尚未完成的 I/O 数量，供热拔设备生命周期保护使用。 */
    atomic_uint io_inflight;
    /* 所有已提交但尚未完成的请求，受设备注册表锁保护。 */
    list_head_t io_requests;
} device_t;

typedef struct driver {
    object_header_t object;
    const char *name;
    const void *match_table;
    uint32_t flags;

    kstatus_t (*probe)(device_t *);
    void (*remove)(device_t *);

    struct module *module;
    uint32_t api_version;
    uint32_t struct_size;
} driver_t;

kstatus_t device_register(device_t *dev);
void device_unregister(device_t *dev);
/* 返回一个拥有引用的设备对象；调用者完成后必须 object_put。 */
kstatus_t device_get_by_id(uint64_t device_id, device_t **out);
/* 按用户可见的紧凑索引返回一个拥有引用的设备对象。 */
kstatus_t device_get_by_index(uint32_t index, device_t **out);
kstatus_t device_suspend(device_t *dev);
kstatus_t device_suspend_timeout(device_t *dev, uint64_t timeout_ns);
kstatus_t device_resume(device_t *dev);
kstatus_t device_resume_timeout(device_t *dev, uint64_t timeout_ns);
kstatus_t device_reset(device_t *dev, uint32_t level);
kstatus_t device_set_power_timeout(device_t *dev, uint64_t timeout_ns);
bool device_io_begin(device_t *dev, io_request_t *req);
void device_io_end(device_t *dev, io_request_t *req);
kstatus_t driver_register(driver_t *drv);
void driver_unregister(driver_t *drv);
