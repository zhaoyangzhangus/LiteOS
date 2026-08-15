#ifndef LITEOS_VFS_H
#define LITEOS_VFS_H

#include "uefi.h"
#include "security.h"

#define LITEOS_VFS_PATH_LENGTH 256U
#define LITEOS_VFS_MOUNT_COUNT  8U
#define LITEOS_RAMFS_FILE_COUNT 16U
#define LITEOS_RAMFS_FILE_SIZE  4096U

typedef struct LITEOS_VFS_NODE LITEOS_VFS_NODE;
typedef struct LITEOS_VFS_MOUNT LITEOS_VFS_MOUNT;

typedef BOOLEAN (*LITEOS_VFS_READ)(LITEOS_VFS_NODE *node, UINT64 offset,
                                  VOID *buffer, UINT32 capacity, UINT32 *read_size);
typedef BOOLEAN (*LITEOS_VFS_WRITE)(LITEOS_VFS_NODE *node, UINT64 offset,
                                   const VOID *buffer, UINT32 size, UINT32 *written_size);
typedef BOOLEAN (*LITEOS_VFS_CLOSE_NODE)(LITEOS_VFS_NODE *node);
typedef BOOLEAN (*LITEOS_VFS_LOOKUP)(VOID *filesystem_context, const CHAR8 *path,
                                    LITEOS_VFS_NODE *node);

typedef struct {
    LITEOS_VFS_READ Read;
    LITEOS_VFS_WRITE Write;
    LITEOS_VFS_CLOSE_NODE Close;
} LITEOS_VFS_FILE_OPERATIONS;

struct LITEOS_VFS_NODE {
    UINT32 Type;
    UINT64 Size;
    VOID *FilesystemContext;
    VOID *FileContext;
    LITEOS_SECURITY_DESCRIPTOR *SecurityDescriptor;
    const LITEOS_VFS_FILE_OPERATIONS *Operations;
};

struct LITEOS_VFS_MOUNT {
    BOOLEAN Mounted;
    CHAR8 Prefix[LITEOS_VFS_PATH_LENGTH];
    LITEOS_VFS_LOOKUP Lookup;
    VOID *FilesystemContext;
};

typedef struct {
    LITEOS_VFS_MOUNT Mounts[LITEOS_VFS_MOUNT_COUNT];
    UINT32 MountCount;
    LITEOS_SECURITY_TOKEN SecurityToken;
    BOOLEAN HasSecurityToken;
    BOOLEAN Initialized;
} LITEOS_VFS_MANAGER;

typedef struct {
    LITEOS_VFS_NODE Node;
    LITEOS_VFS_MANAGER *Manager;
    UINT64 Position;
    UINT32 GrantedAccess;
    BOOLEAN Opened;
} LITEOS_FILE;

typedef struct {
    CHAR8 Path[LITEOS_VFS_PATH_LENGTH];
    UINT8 Data[LITEOS_RAMFS_FILE_SIZE];
    UINT32 Size;
    LITEOS_SECURITY_DESCRIPTOR *SecurityDescriptor;
    BOOLEAN Used;
} LITEOS_RAMFS_FILE;

typedef struct {
    LITEOS_RAMFS_FILE Files[LITEOS_RAMFS_FILE_COUNT];
} LITEOS_RAMFS;

BOOLEAN liteos_vfs_init(LITEOS_VFS_MANAGER *manager);
BOOLEAN liteos_vfs_mount(LITEOS_VFS_MANAGER *manager, const CHAR8 *prefix,
                         LITEOS_VFS_LOOKUP lookup, VOID *filesystem_context);
BOOLEAN liteos_vfs_set_security_token(LITEOS_VFS_MANAGER *manager,
                                       const LITEOS_SECURITY_TOKEN *token);
BOOLEAN liteos_vfs_unmount(LITEOS_VFS_MANAGER *manager, const CHAR8 *prefix);
BOOLEAN liteos_vfs_open(LITEOS_VFS_MANAGER *manager, const CHAR8 *path,
                        LITEOS_FILE *file);
BOOLEAN liteos_vfs_open_access(LITEOS_VFS_MANAGER *manager, const CHAR8 *path,
                               UINT32 desired_access, LITEOS_FILE *file);
BOOLEAN liteos_vfs_close(LITEOS_FILE *file);
BOOLEAN liteos_vfs_read(LITEOS_FILE *file, VOID *buffer, UINT32 capacity,
                        UINT32 *read_size);
BOOLEAN liteos_vfs_write(LITEOS_FILE *file, const VOID *buffer, UINT32 size,
                         UINT32 *written_size);

BOOLEAN liteos_ramfs_init(LITEOS_RAMFS *ramfs);
BOOLEAN liteos_ramfs_create_file(LITEOS_RAMFS *ramfs, const CHAR8 *path,
                                 const VOID *data, UINT32 size);
BOOLEAN liteos_ramfs_set_security_descriptor(
    LITEOS_RAMFS *ramfs, const CHAR8 *path,
    LITEOS_SECURITY_DESCRIPTOR *descriptor);
BOOLEAN liteos_ramfs_lookup(VOID *filesystem_context, const CHAR8 *path,
                            LITEOS_VFS_NODE *node);

#endif
