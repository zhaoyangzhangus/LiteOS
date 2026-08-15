#pragma once

#include "abi.h"

#define OS_NET_STATUS_HARDWARE_PRESENT (1U << 0)
#define OS_NET_STATUS_LINK_UP          (1U << 1)
#define OS_NET_STATUS_IPV6_CONFIGURED  (1U << 2)

typedef struct os_net_status {
    os_versioned_header_t hdr;
    uint32_t flags;
    uint32_t ipv4_address;
    uint32_t ipv4_gateway;
    uint8_t ipv4_prefix_length;
    uint8_t reserved[3];
    uint8_t ipv6_address[16];
    uint64_t link_transitions;
    uint64_t reset_count;
    /* DHCP 客户端构造 chaddr/client-id 所需的硬件地址。 */
    uint8_t mac[6];
    uint8_t mac_reserved[2];
} os_net_status_t;

typedef struct os_net_set_ipv4_config {
    os_versioned_header_t hdr;
    uint32_t address;
    uint32_t gateway;
    uint8_t prefix_length;
    uint8_t reserved[3];
} os_net_set_ipv4_config_t;
