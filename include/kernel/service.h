#pragma once

#include <kernel/base.h>
#include <kernel/spinlock.h>

#define SERVICE_MANAGER_MAX_SERVICES 32U

typedef enum service_state {
    SERVICE_STOPPED = 0,
    SERVICE_STARTING,
    SERVICE_ACTIVE,
    SERVICE_STOPPING,
    SERVICE_FAILED,
} service_state_t;

typedef kstatus_t (*service_start_fn)(void *context);
typedef void (*service_stop_fn)(void *context);

typedef struct service {
    const char *name;
    service_start_fn start;
    service_stop_fn stop;
    void *context;
    uint32_t restart_limit;
    uint32_t restart_count;
    atomic_uint state;
    bool registered;
} service_t;

bool service_manager_init(void);
kstatus_t service_register(const char *name, service_start_fn start,
                           service_stop_fn stop, void *context,
                           uint32_t restart_limit, service_t **out);
kstatus_t service_unregister(service_t *service);
kstatus_t service_start(service_t *service);
kstatus_t service_stop(service_t *service);
kstatus_t service_mark_failed(service_t *service);
uint32_t service_recover_failed(uint32_t budget);
service_state_t service_get_state(const service_t *service);
bool service_manager_self_test(void);
