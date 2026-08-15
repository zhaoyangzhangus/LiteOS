#pragma once
#include "base.h"
#include "object.h"
#include "refcount.h"

typedef uint64_t capability_mask_t;

typedef struct security_descriptor {
    refcount_t refs;
    uint32_t owner_uid;
    uint32_t owner_gid;
    void *acl;
    uint32_t acl_count;
    uint32_t flags;
} security_descriptor_t;

typedef struct security_token {
    object_header_t object;
    uint32_t uid;
    uint32_t gid;
    uint64_t groups;
    uint64_t privileges;
    capability_mask_t capabilities;
    uint32_t flags;
} security_token_t;

kstatus_t security_check_access(const security_token_t *token,
                                const security_descriptor_t *sd,
                                uint32_t desired,
                                uint32_t *granted);
