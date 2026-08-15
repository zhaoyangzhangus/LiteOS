#pragma once

#include <kernel/base.h>

#define WATCHDOG_MAX_CLIENTS 32U

typedef enum watchdog_state {
    WATCHDOG_STOPPED = 0,
    WATCHDOG_RUNNING,
    WATCHDOG_EXPIRED,
} watchdog_state_t;

typedef void (*watchdog_expire_fn)(void *context);

/* 软件看门狗客户端；超时回调始终在 deferred 上下文中执行。 */
typedef struct watchdog_client {
    const char *name;
    uint64_t timeout_tsc;
    uint64_t deadline_tsc;
    watchdog_expire_fn expire;
    void *context;
    uint32_t expiration_count;
    atomic_uint state;
    bool registered;
    bool queued;
    bool notified;
} watchdog_client_t;

bool watchdog_manager_init(void);
kstatus_t watchdog_register(const char *name, uint64_t timeout_ns,
                           watchdog_expire_fn expire, void *context,
                           watchdog_client_t **out);
kstatus_t watchdog_unregister(watchdog_client_t *client);
kstatus_t watchdog_start(watchdog_client_t *client);
kstatus_t watchdog_stop(watchdog_client_t *client);
kstatus_t watchdog_kick(watchdog_client_t *client);
uint32_t watchdog_poll(uint64_t now_tsc);
watchdog_state_t watchdog_get_state(const watchdog_client_t *client);
uint32_t watchdog_expiration_count(const watchdog_client_t *client);
bool watchdog_self_test(void);
