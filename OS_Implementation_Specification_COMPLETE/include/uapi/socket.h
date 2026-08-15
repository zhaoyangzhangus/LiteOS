#pragma once
#include "abi.h"

enum os_address_family {
    OS_AF_INET4 = 2,
    OS_AF_INET6 = 10,
};

enum os_socket_type {
    OS_SOCK_STREAM = 1,
    OS_SOCK_DGRAM = 2,
};

/* IPv6 端点使用网络字节序地址，端口字段沿用现有主机端整数 ABI。 */
typedef struct os_socket_ipv6_endpoint {
    uint8_t address[16];
    uint16_t port;
    uint16_t reserved;
} os_socket_ipv6_endpoint_t;

/* 异步发送只保存复制后的负载；用户缓冲在系统调用返回后即可复用。 */
typedef struct os_socket_async_send {
    os_versioned_header_t hdr;
    os_handle_t socket;
    os_handle_t completion_port;
    uint64_t user_key;
    uint64_t buffer;
    uint64_t length;
    uint32_t address;
    uint16_t port;
    uint16_t flags;
} os_socket_async_send_t;

/* IPv6 异步发送单独使用 16 字节地址，避免复用 IPv4 结构造成截断。 */
typedef struct os_socket_ipv6_async_send {
    os_versioned_header_t hdr;
    os_handle_t socket;
    os_handle_t completion_port;
    uint64_t user_key;
    uint64_t buffer;
    uint64_t length;
    uint8_t address[16];
    uint16_t port;
    uint16_t flags;
} os_socket_ipv6_async_send_t;
