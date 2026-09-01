#pragma once

#include <kernel/kmem.h>
#include <kernel/vfs.h>

#define VFS_PATH_LIMIT 256U
#define VFS_FAT32_MOUNT_LIMIT 32U

typedef struct {
    char prefix[VFS_PATH_LIMIT];
    LITEOS_FAT32 *filesystem;
    bool mounted;
} vfs_fat32_mount_t;

extern vfs_fat32_mount_t g_fat32_mounts[VFS_FAT32_MOUNT_LIMIT];

/* Return the longest matching mount and its path relative to that mount. */
vfs_fat32_mount_t *vfs_fat32_mount_for_path(const char *path,
                                             char relative[VFS_PATH_LIMIT]);

/* VFS-private backend state.  The public header exposes only callback types;
 * object and page-cache ownership stays inside this directory. */
typedef struct vfs_backend_node {
    char path[VFS_PATH_LIMIT];
    uint8_t *data;
    uint64_t size;
    uint32_t mode;
    bool directory;
    bool mutable_node;
    bool detached;
    bool fat32_node;
    vfs_backend_read_t read;
    vfs_backend_write_t write;
    vfs_backend_fsync_t fsync;
    vfs_backend_truncate_t truncate;
    void *context;
    spinlock_t io_lock;
} vfs_backend_node_t;

typedef struct vfs_cached_page {
    uint64_t index;
    page_t *page;
    bool dirty;
    struct vfs_cached_page *next;
} vfs_cached_page_t;

typedef struct vfs_page_cache {
    vfs_cached_page_t *head;
} vfs_page_cache_t;

static inline void vfs_backend_lock(vfs_backend_node_t *backend) {
    while (atomic_exchange_explicit(&backend->io_lock.state, 1U,
                                    memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
}

static inline void vfs_backend_unlock(vfs_backend_node_t *backend) {
    atomic_store_explicit(&backend->io_lock.state, 0U,
                          memory_order_release);
}

static inline void vfs_vnode_lock(vnode_t *vnode) {
    while (atomic_exchange_explicit(&vnode->lock.state, 1U,
                                    memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
}

static inline void vfs_vnode_unlock(vnode_t *vnode) {
    atomic_store_explicit(&vnode->lock.state, 0U,
                          memory_order_release);
}

size_t vfs_string_length(const char *text);
bool vfs_fat32_relative_path(const char *path, char relative[VFS_PATH_LIMIT]);
kstatus_t vfs_register_node(vfs_backend_node_t *backend);

kstatus_t vfs_memory_backend_read(void *context, uint64_t offset,
                                   void *buffer, size_t length,
                                   uint64_t *bytes);
kstatus_t vfs_memory_backend_write(void *context, uint64_t offset,
                                    const void *buffer, size_t length,
                                    uint64_t *bytes);
kstatus_t vfs_memory_backend_truncate(void *context, uint64_t size);
kstatus_t vfs_fat32_backend_read(void *context, uint64_t offset,
                                  void *buffer, size_t length,
                                  uint64_t *bytes);
kstatus_t vfs_fat32_backend_write(void *context, uint64_t offset,
                                   const void *buffer, size_t length,
                                   uint64_t *bytes);
kstatus_t vfs_fat32_backend_fsync(void *context);
kstatus_t vfs_fat32_backend_truncate(void *context, uint64_t size);
kstatus_t vfs_fat32_backend_prepare_unlink(vfs_backend_node_t *backend);
void vfs_fat32_backend_commit_unlink(vfs_backend_node_t *backend);
void vfs_fat32_backend_abort_unlink(vfs_backend_node_t *backend);

kstatus_t vfs_page_cache_flush(vnode_t *vnode);
void vfs_page_cache_destroy(vnode_t *vnode);
kstatus_t vfs_truncate_node(vnode_t *vnode, uint64_t size);
kstatus_t vfs_file_page_get(vnode_t *vnode, uint64_t page_index,
                            struct page **out);
void vfs_file_page_mark_dirty(vnode_t *vnode, uint64_t page_index);
void vfs_page_cache_copy_in(vnode_t *vnode, uint64_t offset,
                            const uint8_t *source, size_t length);
