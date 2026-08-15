#pragma once
#include "../../OS_Implementation_Specification_COMPLETE/include/uapi/socket.h"

/* LiteOS ABI 1 的 IPv4 socket 参数使用主机端整数，协议层负责 wire 字节序。 */
typedef struct os_socket_ipv4_endpoint {
    uint32_t address;
    uint16_t port;
    uint16_t reserved;
} os_socket_ipv4_endpoint_t;
