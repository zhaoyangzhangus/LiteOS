#pragma once

#include <kernel/base.h>

/* 网络管理器只维护链路和地址状态；DHCP/DNS 等策略由用户态服务负责。 */
typedef struct net_manager_status {
    bool initialized;
    bool hardware_present;
    bool link_up;
    bool ipv6_configured;
    uint32_t ipv4_address;
    uint8_t ipv4_prefix_length;
    uint8_t reserved[3];
    uint32_t ipv4_gateway;
    uint8_t ipv6_address[16];
    uint64_t link_transitions;
    uint64_t reset_count;
    uint8_t mac[6];
    uint8_t mac_reserved[2];
} net_manager_status_t;

bool net_manager_init(void);
void net_manager_poll(void);
bool net_manager_get_status(net_manager_status_t *status);
bool net_manager_ready(void);
bool net_manager_self_test(void);
