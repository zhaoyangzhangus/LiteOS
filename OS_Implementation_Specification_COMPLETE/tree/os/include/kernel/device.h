#pragma once
#include "base.h"
#include "object.h"
#include "list.h"
#include "spinlock.h"
#include "io.h"

struct bus_type;
struct driver;
struct module;

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
} device_t;

typedef struct driver {
    object_header_t object;
    const char *name;
    const void *match_table;
    uint32_t flags;

    kstatus_t (*probe)(device_t *);
    void (*remove)(device_t *);

    struct module *module;
} driver_t;

kstatus_t device_register(device_t *dev);
void device_unregister(device_t *dev);
kstatus_t driver_register(driver_t *drv);
void driver_unregister(driver_t *drv);
