#pragma once
#include "base.h"
#include "object.h"
#include "refcount.h"

typedef uint64_t capability_mask_t;

#define KOBJECT_TYPE_SECURITY_TOKEN 0x0113U
#define SECURITY_CAPABILITY_SYSTEM_ADMIN (1ULL << 0)

enum security_principal_type {
    SECURITY_PRINCIPAL_OWNER = 1,
    SECURITY_PRINCIPAL_USER,
    SECURITY_PRINCIPAL_GROUP,
    SECURITY_PRINCIPAL_EVERYONE,
};

typedef struct security_acl_entry {
    uint32_t principal_type;
    uint32_t principal_id;
    uint32_t permissions;
    uint32_t reserved;
} security_acl_entry_t;

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
kstatus_t security_token_create(uint32_t uid, uint32_t gid, uint64_t groups,
                                uint64_t privileges, capability_mask_t capabilities,
                                security_token_t **out);
bool security_core_self_test(void);
