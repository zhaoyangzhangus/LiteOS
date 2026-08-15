#pragma once
#include "abi.h"

typedef struct os_token_info {
    os_versioned_header_t hdr;
    uint32_t uid;
    uint32_t gid;
    uint64_t groups;
    uint64_t privileges;
    uint64_t capabilities;
    uint32_t flags;
    uint32_t reserved;
} os_token_info_t;
