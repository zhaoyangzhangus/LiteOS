#ifndef LITEOS_OBJECT_H
#define LITEOS_OBJECT_H

#include "slab.h"
#include "security.h"

typedef UINT32 LITEOS_HANDLE;

enum {
    LITEOS_OBJECT_PROCESS = 1U,
    LITEOS_OBJECT_THREAD,
    LITEOS_OBJECT_FILE,
    LITEOS_OBJECT_DEVICE,
    LITEOS_OBJECT_MEMORY_SECTION,
    LITEOS_OBJECT_EVENT,
    LITEOS_OBJECT_SEMAPHORE,
    LITEOS_OBJECT_MUTEX,
    LITEOS_OBJECT_TIMER,
    LITEOS_OBJECT_SOCKET,
    LITEOS_OBJECT_GPU_CONTEXT,
};

typedef struct {
    UINT32 Type;
    UINT32 ReferenceCount;
    UINT64 SecurityDescriptor;
} LITEOS_OBJECT_HEADER;

typedef struct {
    LITEOS_OBJECT_HEADER Header;
    UINT8 Body[128];
} LITEOS_OBJECT;

#define LITEOS_HANDLE_TABLE_SIZE 256U

typedef struct {
    LITEOS_OBJECT *Object;
    UINT32 Generation;
} LITEOS_HANDLE_ENTRY;

typedef struct {
    LITEOS_SLAB_CACHE ObjectCache;
    LITEOS_HANDLE_ENTRY Handles[LITEOS_HANDLE_TABLE_SIZE];
    LITEOS_SECURITY_TOKEN SecurityToken;
    BOOLEAN HasSecurityToken;
    BOOLEAN Initialized;
} LITEOS_OBJECT_MANAGER;

BOOLEAN liteos_object_manager_init(LITEOS_OBJECT_MANAGER *manager);
LITEOS_OBJECT *liteos_object_create(LITEOS_OBJECT_MANAGER *manager, UINT32 type);
VOID liteos_object_reference(LITEOS_OBJECT *object);
VOID liteos_object_release(LITEOS_OBJECT_MANAGER *manager, LITEOS_OBJECT *object);

BOOLEAN liteos_object_set_security_descriptor(
    LITEOS_OBJECT *object, LITEOS_SECURITY_DESCRIPTOR *descriptor);
BOOLEAN liteos_object_manager_set_token(LITEOS_OBJECT_MANAGER *manager,
                                        const LITEOS_SECURITY_TOKEN *token);

LITEOS_HANDLE liteos_handle_open(LITEOS_OBJECT_MANAGER *manager, LITEOS_OBJECT *object);
LITEOS_OBJECT *liteos_handle_get(LITEOS_OBJECT_MANAGER *manager, LITEOS_HANDLE handle);
LITEOS_HANDLE liteos_handle_open_access(LITEOS_OBJECT_MANAGER *manager,
                                        LITEOS_OBJECT *object,
                                        UINT32 desired_access);
LITEOS_OBJECT *liteos_handle_get_access(LITEOS_OBJECT_MANAGER *manager,
                                        LITEOS_HANDLE handle,
                                        UINT32 desired_access);
BOOLEAN liteos_handle_close(LITEOS_OBJECT_MANAGER *manager, LITEOS_HANDLE handle);

VOID liteos_object_manager_destroy(LITEOS_OBJECT_MANAGER *manager);

#endif
