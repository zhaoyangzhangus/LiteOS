#include "object.h"

static VOID memory_zero(UINT8 *memory, UINT64 size) {
    while (size-- != 0) *memory++ = 0;
}

static UINT32 handle_index(LITEOS_HANDLE handle) {
    if (handle == 0) return (UINT32)-1;
    return handle & (LITEOS_HANDLE_TABLE_SIZE - 1U);
}

static UINT32 handle_generation(LITEOS_HANDLE handle) {
    return handle >> 8;
}

static LITEOS_HANDLE make_handle(UINT32 index, UINT32 generation) {
    return (LITEOS_HANDLE)((generation << 8) | index);
}

static BOOLEAN object_access_allowed(const LITEOS_OBJECT_MANAGER *manager,
                                     const LITEOS_OBJECT *object,
                                     UINT32 desired_access) {
    const LITEOS_SECURITY_DESCRIPTOR *descriptor;
    if (manager == 0 || object == 0 || desired_access == 0) return 0;
    if (object->Header.SecurityDescriptor == 0) return 1;
    if (!manager->HasSecurityToken) return 0;
    descriptor = (const LITEOS_SECURITY_DESCRIPTOR *)(uintptr_t)
        object->Header.SecurityDescriptor;
    return liteos_security_access_check(&manager->SecurityToken, descriptor,
                                        desired_access);
}

BOOLEAN liteos_object_manager_init(LITEOS_OBJECT_MANAGER *manager) {
    if (manager == 0 || !liteos_slab_cache_init(&manager->ObjectCache,
                                                sizeof(LITEOS_OBJECT), 16U)) return 0;
    for (UINT32 i = 0; i < LITEOS_HANDLE_TABLE_SIZE; ++i) {
        manager->Handles[i].Object = 0;
        manager->Handles[i].Generation = 1U;
    }
    manager->SecurityToken.UserId = 0;
    manager->SecurityToken.GroupId = 0;
    manager->SecurityToken.Capabilities = 0;
    manager->HasSecurityToken = 0;
    manager->Initialized = 1;
    return 1;
}

LITEOS_OBJECT *liteos_object_create(LITEOS_OBJECT_MANAGER *manager, UINT32 type) {
    if (manager == 0 || !manager->Initialized || type == 0) return 0;
    LITEOS_OBJECT *object = (LITEOS_OBJECT *)liteos_slab_alloc(&manager->ObjectCache);
    if (object == 0) return 0;
    memory_zero((UINT8 *)object, sizeof(*object));
    object->Header.Type = type;
    object->Header.ReferenceCount = 1U;
    return object;
}

VOID liteos_object_reference(LITEOS_OBJECT *object) {
    if (object != 0) __atomic_add_fetch(&object->Header.ReferenceCount, 1U, __ATOMIC_RELAXED);
}

VOID liteos_object_release(LITEOS_OBJECT_MANAGER *manager, LITEOS_OBJECT *object) {
    if (manager == 0 || object == 0) return;
    if (__atomic_sub_fetch(&object->Header.ReferenceCount, 1U, __ATOMIC_ACQ_REL) == 0) {
        liteos_slab_free(&manager->ObjectCache, object);
    }
}

BOOLEAN liteos_object_set_security_descriptor(
    LITEOS_OBJECT *object, LITEOS_SECURITY_DESCRIPTOR *descriptor) {
    if (object == 0) return 0;
    object->Header.SecurityDescriptor = (UINT64)(uintptr_t)descriptor;
    return 1;
}

BOOLEAN liteos_object_manager_set_token(LITEOS_OBJECT_MANAGER *manager,
                                        const LITEOS_SECURITY_TOKEN *token) {
    if (manager == 0 || !manager->Initialized) return 0;
    if (token == 0) {
        manager->HasSecurityToken = 0;
        return 1;
    }
    manager->SecurityToken = *token;
    manager->HasSecurityToken = 1;
    return 1;
}

LITEOS_HANDLE liteos_handle_open_access(LITEOS_OBJECT_MANAGER *manager,
                                        LITEOS_OBJECT *object,
                                        UINT32 desired_access) {
    if (manager == 0 || object == 0 || !manager->Initialized ||
        !object_access_allowed(manager, object, desired_access)) return 0;
    for (UINT32 i = 0; i < LITEOS_HANDLE_TABLE_SIZE; ++i) {
        LITEOS_HANDLE_ENTRY *entry = &manager->Handles[i];
        if (entry->Object == 0) {
            entry->Object = object;
            liteos_object_reference(object);
            return make_handle(i, entry->Generation);
        }
    }
    return 0;
}

LITEOS_HANDLE liteos_handle_open(LITEOS_OBJECT_MANAGER *manager,
                                  LITEOS_OBJECT *object) {
    return liteos_handle_open_access(manager, object, LITEOS_ACCESS_READ);
}

LITEOS_OBJECT *liteos_handle_get_access(LITEOS_OBJECT_MANAGER *manager,
                                        LITEOS_HANDLE handle,
                                        UINT32 desired_access) {
    if (manager == 0 || !manager->Initialized) return 0;
    UINT32 index = handle_index(handle);
    if (index >= LITEOS_HANDLE_TABLE_SIZE || manager->Handles[index].Object == 0 ||
        handle_generation(handle) != manager->Handles[index].Generation) return 0;
    LITEOS_OBJECT *object = manager->Handles[index].Object;
    if (!object_access_allowed(manager, object, desired_access)) return 0;
    liteos_object_reference(object);
    return object;
}

LITEOS_OBJECT *liteos_handle_get(LITEOS_OBJECT_MANAGER *manager,
                                  LITEOS_HANDLE handle) {
    return liteos_handle_get_access(manager, handle, LITEOS_ACCESS_READ);
}

BOOLEAN liteos_handle_close(LITEOS_OBJECT_MANAGER *manager, LITEOS_HANDLE handle) {
    if (manager == 0 || !manager->Initialized) return 0;
    UINT32 index = handle_index(handle);
    if (index >= LITEOS_HANDLE_TABLE_SIZE || manager->Handles[index].Object == 0 ||
        handle_generation(handle) != manager->Handles[index].Generation) return 0;
    LITEOS_OBJECT *object = manager->Handles[index].Object;
    manager->Handles[index].Object = 0;
    ++manager->Handles[index].Generation;
    if (manager->Handles[index].Generation == 0) manager->Handles[index].Generation = 1U;
    liteos_object_release(manager, object);
    return 1;
}

VOID liteos_object_manager_destroy(LITEOS_OBJECT_MANAGER *manager) {
    if (manager == 0 || !manager->Initialized) return;
    for (UINT32 i = 0; i < LITEOS_HANDLE_TABLE_SIZE; ++i) {
        if (manager->Handles[i].Object != 0) {
            liteos_object_release(manager, manager->Handles[i].Object);
            manager->Handles[i].Object = 0;
        }
    }
    liteos_slab_cache_destroy(&manager->ObjectCache);
    manager->Initialized = 0;
}
