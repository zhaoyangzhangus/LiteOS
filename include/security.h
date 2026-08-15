#ifndef LITEOS_SECURITY_H
#define LITEOS_SECURITY_H

#include "uefi.h"

#define LITEOS_SECURITY_MAX_ACL 16U

enum {
    LITEOS_SECURITY_PRINCIPAL_OWNER = 1U,
    LITEOS_SECURITY_PRINCIPAL_USER,
    LITEOS_SECURITY_PRINCIPAL_GROUP,
    LITEOS_SECURITY_PRINCIPAL_EVERYONE,
};

enum {
    LITEOS_ACCESS_READ    = 1U << 0,
    LITEOS_ACCESS_WRITE   = 1U << 1,
    LITEOS_ACCESS_EXECUTE = 1U << 2,
    LITEOS_ACCESS_ADMIN   = 1U << 3,
};

#define LITEOS_CAPABILITY_SYSTEM_ADMIN (1ULL << 0)

typedef struct {
    UINT32 UserId;
    UINT32 GroupId;
    UINT64 Capabilities;
} LITEOS_SECURITY_TOKEN;

typedef struct {
    UINT32 PrincipalType;
    UINT32 PrincipalId;
    UINT32 Permissions;
} LITEOS_SECURITY_ACL_ENTRY;

typedef struct {
    UINT32 OwnerUserId;
    UINT32 OwnerGroupId;
    UINT32 EntryCount;
    LITEOS_SECURITY_ACL_ENTRY Entries[LITEOS_SECURITY_MAX_ACL];
} LITEOS_SECURITY_DESCRIPTOR;

BOOLEAN liteos_security_token_init(LITEOS_SECURITY_TOKEN *token,
                                   UINT32 user_id, UINT32 group_id,
                                   UINT64 capabilities);
BOOLEAN liteos_security_descriptor_init(LITEOS_SECURITY_DESCRIPTOR *descriptor,
                                        UINT32 owner_user_id, UINT32 owner_group_id);
BOOLEAN liteos_security_acl_add(LITEOS_SECURITY_DESCRIPTOR *descriptor,
                                UINT32 principal_type, UINT32 principal_id,
                                UINT32 permissions);
BOOLEAN liteos_security_access_check(const LITEOS_SECURITY_TOKEN *token,
                                    const LITEOS_SECURITY_DESCRIPTOR *descriptor,
                                    UINT32 desired_access);

#endif
