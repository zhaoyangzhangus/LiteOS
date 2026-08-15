#include "security.h"

#ifdef LITEOS_KERNEL_BUILD

#include <kernel/kmem.h>
#include <kernel/audit.h>
#include <kernel/security.h>

static void security_token_destroy(void *object) {
    kfree(object);
}

static const object_ops_t g_security_token_ops = {
    .destroy = security_token_destroy,
    .type_name = "SecurityToken",
    .is_signaled = 0,
    .wait_value = 0,
};

static bool security_acl_matches(const security_token_t *token,
                                 const security_descriptor_t *descriptor,
                                 const security_acl_entry_t *entry) {
    if (token == 0 || descriptor == 0 || entry == 0) return false;
    switch (entry->principal_type) {
        case SECURITY_PRINCIPAL_OWNER:
            return token->uid == descriptor->owner_uid;
        case SECURITY_PRINCIPAL_USER:
            return token->uid == entry->principal_id;
        case SECURITY_PRINCIPAL_GROUP:
            return entry->principal_id < 64U &&
                   (token->groups & (1ULL << entry->principal_id)) != 0U;
        case SECURITY_PRINCIPAL_EVERYONE:
            return true;
        default:
            return false;
    }
}

kstatus_t security_token_create(uint32_t uid, uint32_t gid, uint64_t groups,
                                uint64_t privileges, capability_mask_t capabilities,
                                security_token_t **out) {
    if (out == 0) return K_EINVAL;
    security_token_t *token = (security_token_t *)kzalloc(sizeof(*token), 0);
    if (token == 0) return K_ENOMEM;
    refcount_init(&token->object.refs, 1U);
    token->object.type = KOBJECT_TYPE_SECURITY_TOKEN;
    token->object.flags = 0U;
    token->object.ops = &g_security_token_ops;
    token->object.security = 0;
    token->uid = uid;
    token->gid = gid;
    token->groups = groups;
    token->privileges = privileges;
    token->capabilities = capabilities;
    token->flags = 0U;
    *out = token;
    return K_OK;
}

kstatus_t security_check_access(const security_token_t *token,
                                const security_descriptor_t *descriptor,
                                uint32_t desired, uint32_t *granted) {
    uint32_t allowed = 0U;
    if (granted != 0) *granted = 0U;
    if (token == 0 || desired == 0U) return K_EINVAL;
    if (descriptor == 0 ||
        (token->capabilities & SECURITY_CAPABILITY_SYSTEM_ADMIN) != 0U) {
        allowed = desired;
    } else if (descriptor->acl == 0 && token->uid == descriptor->owner_uid) {
        /* ACL 为空时，所有者拥有该对象的默认权限。 */
        allowed = desired;
    } else if (descriptor->acl != 0 && descriptor->acl_count <= 64U) {
        const security_acl_entry_t *entries =
            (const security_acl_entry_t *)descriptor->acl;
        for (uint32_t i = 0; i < descriptor->acl_count; ++i) {
            if (security_acl_matches(token, descriptor, &entries[i])) {
                allowed |= entries[i].permissions;
            }
        }
    }
    if (granted != 0) *granted = allowed & desired;
    if ((allowed & desired) != desired) {
        (void)audit_emit(AUDIT_EVENT_ACCESS_DENIED,
                         descriptor != 0 ? descriptor->owner_uid : 0U,
                         token->uid, token->gid, desired, K_EACCES);
    }
    return (allowed & desired) == desired ? K_OK : K_EACCES;
}

bool security_core_self_test(void) {
    security_token_t *owner = 0;
    security_token_t *guest = 0;
    security_acl_entry_t acl = {
        .principal_type = SECURITY_PRINCIPAL_USER,
        .principal_id = 100U,
        .permissions = 3U,
    };
    security_descriptor_t descriptor = {
        .owner_uid = 100U,
        .owner_gid = 100U,
        .acl = &acl,
        .acl_count = 1U,
        .flags = 0U,
    };
    uint32_t granted = 0U;
    bool success = security_token_create(100U, 100U, 0U, 0U, 0U, &owner) == K_OK &&
                   security_token_create(200U, 200U, 0U, 0U, 0U, &guest) == K_OK &&
                   owner != 0 && guest != 0 &&
                   security_check_access(owner, &descriptor, 3U, &granted) == K_OK &&
                   granted == 3U &&
                   security_check_access(guest, &descriptor, 1U, &granted) == K_EACCES;
    if (owner != 0) object_put(owner);
    if (guest != 0) object_put(guest);
    return success;
}

#endif /* LITEOS_KERNEL_BUILD */

BOOLEAN liteos_security_token_init(LITEOS_SECURITY_TOKEN *token,
                                   UINT32 user_id, UINT32 group_id,
                                   UINT64 capabilities) {
    if (token == 0) return 0;
    token->UserId = user_id;
    token->GroupId = group_id;
    token->Capabilities = capabilities;
    return 1;
}

BOOLEAN liteos_security_descriptor_init(LITEOS_SECURITY_DESCRIPTOR *descriptor,
                                        UINT32 owner_user_id, UINT32 owner_group_id) {
    if (descriptor == 0) return 0;
    descriptor->OwnerUserId = owner_user_id;
    descriptor->OwnerGroupId = owner_group_id;
    descriptor->EntryCount = 0;
    return 1;
}

BOOLEAN liteos_security_acl_add(LITEOS_SECURITY_DESCRIPTOR *descriptor,
                                UINT32 principal_type, UINT32 principal_id,
                                UINT32 permissions) {
    if (descriptor == 0 || descriptor->EntryCount >= LITEOS_SECURITY_MAX_ACL ||
        principal_type < LITEOS_SECURITY_PRINCIPAL_OWNER ||
        principal_type > LITEOS_SECURITY_PRINCIPAL_EVERYONE) return 0;
    LITEOS_SECURITY_ACL_ENTRY *entry = &descriptor->Entries[descriptor->EntryCount++];
    entry->PrincipalType = principal_type;
    entry->PrincipalId = principal_id;
    entry->Permissions = permissions;
    return 1;
}

static BOOLEAN acl_entry_matches(const LITEOS_SECURITY_TOKEN *token,
                                 const LITEOS_SECURITY_DESCRIPTOR *descriptor,
                                 const LITEOS_SECURITY_ACL_ENTRY *entry) {
    switch (entry->PrincipalType) {
        case LITEOS_SECURITY_PRINCIPAL_OWNER:
            return token->UserId == descriptor->OwnerUserId;
        case LITEOS_SECURITY_PRINCIPAL_USER:
            return token->UserId == entry->PrincipalId;
        case LITEOS_SECURITY_PRINCIPAL_GROUP:
            return token->GroupId == entry->PrincipalId;
        case LITEOS_SECURITY_PRINCIPAL_EVERYONE:
            return 1;
        default:
            return 0;
    }
}

BOOLEAN liteos_security_access_check(const LITEOS_SECURITY_TOKEN *token,
                                    const LITEOS_SECURITY_DESCRIPTOR *descriptor,
                                    UINT32 desired_access) {
    if (token == 0 || descriptor == 0 || desired_access == 0) return 0;
    if ((token->Capabilities & LITEOS_CAPABILITY_SYSTEM_ADMIN) != 0) return 1;
    UINT32 allowed = 0;
    for (UINT32 i = 0; i < descriptor->EntryCount; ++i) {
        const LITEOS_SECURITY_ACL_ENTRY *entry = &descriptor->Entries[i];
        if (acl_entry_matches(token, descriptor, entry)) allowed |= entry->Permissions;
    }
    return (allowed & desired_access) == desired_access;
}
