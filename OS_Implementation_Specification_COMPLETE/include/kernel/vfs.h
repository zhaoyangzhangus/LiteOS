#pragma once
#include "base.h"
#include "object.h"
#include "list.h"
#include "rbtree.h"
#include "spinlock.h"

struct page;
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
