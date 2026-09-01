#pragma once
#pragma once
#include "base.h"
#include "object.h"
#include "list.h"
#include "rbtree.h"
#include "spinlock.h"
#include "fat32.h"

struct page;
struct vm_file_ops;
struct vm_area;

typedef struct vnode_ops vnode_ops_t;
typedef struct file_ops file_ops_t;

typedef struct vnode {
    object_header_t object;
    uint64_t inode_id;
    uint32_t mode;
    uint32_t flags;
    uint64_t size;

    const vnode_ops_t *ops;
    void *fs_private;

    rwlock_t lock;
    void *page_cache;
} vnode_t;

typedef struct file {
    object_header_t object;
    vnode_t *vnode;
    const file_ops_t *ops;
    uint64_t position;
    uint32_t flags;
    uint32_t rights;
    void *private_data;
} file_t;

kstatus_t vfs_open(const char __user *path, uint32_t flags, uint32_t mode, file_t **out);
kstatus_t vfs_read(file_t *file, void __user *buf, size_t len, uint64_t *bytes);
kstatus_t vfs_write(file_t *file, const void __user *buf, size_t len, uint64_t *bytes);
kstatus_t vfs_fsync(file_t *file);
void vfs_close(file_t *file);
#include <uapi/file.h>

/* 规范 VFS 的最小打开标志；文件系统后端可以继续扩展更多权限位。 */
#define VFS_OPEN_READ   (1U << 0)
#define VFS_OPEN_WRITE  (1U << 1)
#define VFS_OPEN_CREATE (1U << 2)
#define VFS_OPEN_EXCLUSIVE (1U << 3)
#define VFS_OPEN_TRUNCATE (1U << 4)
#define VFS_OPEN_APPEND (1U << 5)
#define VFS_OPEN_DIRECTORY (1U << 6)

/* 文件对象使用独立的内核对象类型，句柄权限与打开标志保持一致。 */
#define KOBJECT_TYPE_VNODE 0x0103U
#define KOBJECT_TYPE_FILE  0x0104U
#define FILE_RIGHT_READ    VFS_OPEN_READ
#define FILE_RIGHT_WRITE   VFS_OPEN_WRITE

struct page;
/* 统一文件后端使用内核缓冲区，避免文件系统直接接触用户地址。 */
typedef kstatus_t (*vfs_backend_read_t)(void *context, uint64_t offset,
                                        void *buffer, size_t length,
                                        uint64_t *bytes);
typedef kstatus_t (*vfs_backend_write_t)(void *context, uint64_t offset,
                                         const void *buffer, size_t length,
                                         uint64_t *bytes);
typedef kstatus_t (*vfs_backend_fsync_t)(void *context);
typedef kstatus_t (*vfs_backend_truncate_t)(void *context, uint64_t size);

kstatus_t vfs_register_backend_file(const char *path, uint64_t size, uint32_t mode,
                                    vfs_backend_read_t read,
                                    vfs_backend_write_t write,
                                    vfs_backend_fsync_t fsync,
                                    void *context);
kstatus_t vfs_register_backend_file_ex(const char *path, uint64_t size, uint32_t mode,
                                       vfs_backend_read_t read,
                                       vfs_backend_write_t write,
                                       vfs_backend_fsync_t fsync,
                                       vfs_backend_truncate_t truncate,
                                       void *context);
kstatus_t vfs_mount_fat32(const char *prefix, LITEOS_FAT32 *filesystem);
kstatus_t vfs_unmount_fat32(void);
kstatus_t vfs_create_kernel(const char *path, uint32_t mode, bool directory);
kstatus_t vfs_mkdir_kernel(const char *path, uint32_t mode);
kstatus_t vfs_remove_kernel(const char *path);
kstatus_t vfs_open_kernel(const char *path, uint32_t flags, uint32_t mode,
                          file_t **out);
kstatus_t vfs_stat_kernel(const char *path, os_file_info_t *out);
kstatus_t vfs_file_stat(file_t *file, os_file_info_t *out);
kstatus_t vfs_rename_kernel(const char *old_path, const char *new_path);
kstatus_t vfs_enumerate_kernel(const char *path, uint32_t index,
                               os_file_info_t *out);
kstatus_t vfs_stat(const char __user *path, os_file_info_t *out);
kstatus_t vfs_enumerate(const char __user *path, uint32_t index,
                        os_file_info_t *out);
kstatus_t vfs_file_page_get(vnode_t *vnode, uint64_t page_index,
                            struct page **out);
void vfs_file_page_mark_dirty(vnode_t *vnode, uint64_t page_index);
kstatus_t vfs_page_cache_sync(vnode_t *vnode, uint64_t first_page,
                              uint64_t page_count);
const struct vm_file_ops *vfs_vm_file_ops(void);
kstatus_t vfs_read_kernel(file_t *file, void *buf, size_t len, uint64_t *bytes);
kstatus_t vfs_write_kernel(file_t *file, const void *buf, size_t len,
                           uint64_t *bytes);
kstatus_t vfs_seek(file_t *file, int64_t offset, uint32_t whence,
                   uint64_t *position);
kstatus_t vfs_truncate(file_t *file, uint64_t size);
kstatus_t vfs_truncate_kernel(file_t *file, uint64_t size);
kstatus_t vfs_remove(const char __user *path);
kstatus_t vfs_mkdir(const char __user *path, uint32_t mode);
