#pragma once

#include "abi.h"

enum os_port_kind {
    OS_PORT_MESSAGE = 1,
    OS_PORT_COMPLETION = 2,
};

#define OS_PORT_DEFAULT_CAPACITY 32u
#define OS_PORT_MAX_CAPACITY 32u
#define OS_PORT_MAX_MESSAGE_SIZE 256u
