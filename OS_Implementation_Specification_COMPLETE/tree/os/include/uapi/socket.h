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
