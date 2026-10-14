#include <kernel/kmem.h>
#include <kernel/console.h>
#include <kernel/string.h>
#include "internal.h"

#ifdef OS_KERNEL_BUILD

/* REFACTOR_FS_VFS_BACKEND_OWNER: memory/FAT32 adapters and backend registration. */

/* Amortize append writes; size remains the visible EOF, not the allocation. */
static kstatus_t vfs_memory_reserve(vfs_backend_node_t *backend,
                                   uint64_t required, uint8_t **retired) {
    if (retired != 0) *retired = 0;
    uint64_t capacity = backend->data_capacity;
    if (backend->data != 0 && capacity < backend->size) capacity = backend->size;
    if (required <= capacity) return K_OK;
    if (required > SIZE_MAX) return K_EOVERFLOW;
    if (capacity < 4096U) capacity = 4096U;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2U) { capacity = required; break; }
        capacity *= 2U;
    }
    uint8_t *replacement = (uint8_t *)kmalloc((size_t)capacity, 0);
    /* Spare capacity is optional under memory pressure. */
    if (replacement == 0 && capacity != required) {
        capacity = required;
        replacement = (uint8_t *)kmalloc((size_t)capacity, 0);
    }
    if (replacement == 0) return K_ENOMEM;
    if (backend->size != 0U)
        memcpy(replacement, backend->data, (size_t)backend->size);
    /* A write's source may alias this allocation. Its caller releases the
     * retired buffer only after the final memmove has consumed the source. */
    if (retired != 0) *retired = backend->data;
    else kfree(backend->data);
    backend->data = replacement;
    backend->data_capacity = capacity;
    return K_OK;
}

kstatus_t vfs_memory_backend_read(void *context, uint64_t offset,
                                   void *buffer, size_t length,
                                   uint64_t *bytes) {
    vfs_backend_node_t *backend = (vfs_backend_node_t *)context;
    if (backend == 0 || bytes == 0 || (length != 0 && buffer == 0) ||
        offset > backend->size) return K_EINVAL;
    uint64_t count = backend->size - offset;
    if (count > length) count = length;
    if (count != 0U) memmove(buffer, backend->data + offset, (size_t)count);
    *bytes = count;
    return K_OK;
}

kstatus_t vfs_memory_backend_write(void *context, uint64_t offset,
                                    const void *buffer, size_t length,
                                    uint64_t *bytes) {
    vfs_backend_node_t *backend = (vfs_backend_node_t *)context;
    uint64_t required;
    if (backend == 0 || backend->directory || bytes == 0 ||
        (length != 0U && buffer == 0) || offset > UINT64_MAX - length) {
        return K_EINVAL;
    }
    required = offset + length;
    if (length == 0U) { *bytes = 0U; return K_OK; }
    uint8_t *retired = 0;
    if (required > backend->size) {
        kstatus_t status = vfs_memory_reserve(backend, required, &retired);
        if (status != K_OK) return status;
        if (offset > backend->size)
            memset(backend->data + backend->size, 0, (size_t)(offset - backend->size));
        backend->size = required;
    }
    memmove(backend->data + offset, buffer, length);
    if (retired != 0) kfree(retired);
    *bytes = length;
    return K_OK;
}

kstatus_t vfs_memory_backend_truncate(void *context, uint64_t size) {
    vfs_backend_node_t *backend = (vfs_backend_node_t *)context;
    if (backend == 0 || backend->directory || size > SIZE_MAX) return K_EINVAL;
    if (size == backend->size) return K_OK;
    if (size == 0U) {
        kfree(backend->data);
        backend->data = 0;
        backend->size = 0U;
        backend->data_capacity = 0U;
        return K_OK;
    }
    kstatus_t status = vfs_memory_reserve(backend, size, 0);
    if (status != K_OK) return status;
    if (size > backend->size)
        memset(backend->data + backend->size, 0, (size_t)(size - backend->size));
    backend->size = size;
    return K_OK;
}

kstatus_t vfs_fat32_backend_read(void *context, uint64_t offset,
                                  void *buffer, size_t length,
                                  uint64_t *bytes) {
    vfs_backend_node_t *backend = (vfs_backend_node_t *)context;
    char relative[VFS_PATH_LIMIT];
    vfs_fat32_mount_t *mount;
    UINT32 transferred = 0U;
    if (backend != 0 && backend->detached) {
        return vfs_memory_backend_read(backend, offset, buffer, length, bytes);
    }
    if (backend == 0 || bytes == 0 || length > UINT32_MAX) return K_EINVAL;
    if (backend->fat_file != 0) {
        if (!fat32_read_file(backend->fat_file, offset, buffer,
                                    (UINT32)length, &transferred)) {
            return K_EIO;
        }
        *bytes = transferred;
        return K_OK;
    }
    if (
        (mount = vfs_fat32_mount_for_path(backend->path, relative)) == 0 ||
        relative[0] == 0 ||
        !fat32_read_path(mount->filesystem, relative, offset,
                                 buffer, (UINT32)length, &transferred)) return K_EIO;
    *bytes = transferred;
    return K_OK;
}

kstatus_t vfs_fat32_backend_write(void *context, uint64_t offset,
                                   const void *buffer, size_t length,
                                   uint64_t *bytes) {
    vfs_backend_node_t *backend = (vfs_backend_node_t *)context;
    char relative[VFS_PATH_LIMIT];
    vfs_fat32_mount_t *mount;
    UINT32 transferred = 0U;
    if (backend != 0 && backend->detached) {
        return vfs_memory_backend_write(backend, offset, buffer, length, bytes);
    }
    /* Also invalidate snapshots after a failed/partial write attempt. */
    if (backend != 0 && length != 0U) ++backend->content_revision;
    if (backend != 0 && backend->fat_file != 0) {
        if (bytes == 0 || length > UINT32_MAX ||
            !fat32_write_file(backend->fat_file, offset, buffer,
                                     (UINT32)length, &transferred)) return K_EIO;
        *bytes = transferred;
        backend->size = backend->fat_file->Size;
        return K_OK;
    }
    if (backend == 0 || bytes == 0 || length > UINT32_MAX ||
        (mount = vfs_fat32_mount_for_path(backend->path, relative)) == 0 ||
        relative[0] == 0 ||
        !fat32_write_path(mount->filesystem, relative, offset,
                                 buffer, (UINT32)length, &transferred)) return K_EIO;
    *bytes = transferred;
    if (offset <= UINT64_MAX - transferred && offset + transferred > backend->size) {
        backend->size = offset + transferred;
    }
    return K_OK;
}

kstatus_t vfs_fat32_backend_fsync(void *context) {
    vfs_backend_node_t *backend = (vfs_backend_node_t *)context;
    char relative[VFS_PATH_LIMIT];
    vfs_fat32_mount_t *mount;
    if (backend != 0 && backend->detached) return K_OK;
    if (backend != 0 && backend->fat_file != 0) {
        /* Follow the open inode's filesystem, not a mutable namespace path.
         * This also avoids repeated mount scans for ordinary file fsync. */
        OS_FAT32_FILE *file = backend->fat_file;
        if (!file->Used || file->FileSystem == 0) return K_EIO;
        return fat32_sync(file->FileSystem) ? K_OK : K_EIO;
    }
    if (backend == 0 ||
        (mount = vfs_fat32_mount_for_path(backend->path, relative)) == 0) {
        return K_EIO;
    }
    return fat32_sync(mount->filesystem) ? K_OK : K_EIO;
}

kstatus_t vfs_fat32_backend_truncate(void *context, uint64_t size) {
    vfs_backend_node_t *backend = (vfs_backend_node_t *)context;
    char relative[VFS_PATH_LIMIT];
    vfs_fat32_mount_t *mount;
    if (backend != 0 && backend->detached) {
        return vfs_memory_backend_truncate(backend, size);
    }
    if (backend != 0 && size != backend->size) ++backend->content_revision;
    if (backend != 0 && backend->fat_file != 0) {
        if (!fat32_truncate_file(backend->fat_file, size)) return K_EIO;
        backend->size = backend->fat_file->Size;
        return K_OK;
    }
    if (backend == 0 ||
        (mount = vfs_fat32_mount_for_path(backend->path, relative)) == 0 ||
        relative[0] == 0 || !fat32_truncate_path(mount->filesystem,
                                                        relative, size)) return K_EIO;
    backend->size = size;
    return K_OK;
}

/* FAT path helpers reopen by name, while POSIX unlink keeps an open inode
 * alive.  Snapshot a cached vnode before removing its directory entry so
 * existing handles and lazy file mappings keep seeing the same file. */
kstatus_t vfs_fat32_backend_prepare_unlink(vfs_backend_node_t *backend) {
    char relative[VFS_PATH_LIMIT];
    uint8_t *snapshot = 0;
    uint64_t offset = 0U;
    uint64_t size;
    uint64_t revision;
    OS_FAT32_FILE *file;
    vfs_fat32_mount_t *mount = 0;
    if (backend == 0) return K_OK;
    vfs_backend_lock(backend);
    if (backend->directory || backend->detached) {
        vfs_backend_unlock(backend);
        return K_OK;
    }
    if (backend->data != 0 || backend->unlink_prepared) {
        vfs_backend_unlock(backend);
        return K_EBUSY;
    }
    size = backend->size;
    revision = backend->content_revision;
    file = backend->fat_file;
    vfs_backend_unlock(backend);
    if (size > SIZE_MAX) return K_EINVAL;
    if (file == 0 &&
        ((mount = vfs_fat32_mount_for_path(backend->path, relative)) == 0 ||
         relative[0] == 0U)) return K_EINVAL;
    if (size != 0U) {
        snapshot = (uint8_t *)kmalloc((size_t)size, 0);
        if (snapshot == 0) return K_ENOMEM;
        while (offset < size) {
            uint64_t remaining = size - offset;
            UINT32 capacity = remaining > 1024U * 1024U ? 1024U * 1024U :
                              (UINT32)remaining;
            UINT32 transferred = 0U;
            /* Preserve the already-open inode identity; the namespace may
             * have changed since this vnode was acquired. */
            BOOLEAN success = file != 0 ?
                fat32_read_file(file, offset,
                                        snapshot + offset, capacity, &transferred) :
                fat32_read_path(mount->filesystem, relative, offset,
                                        snapshot + offset, capacity, &transferred);
            if (!success ||
                transferred != capacity) {
                kfree(snapshot);
                return K_EIO;
            }
            offset += transferred;
        }
    }
    vfs_backend_lock(backend);
    if (backend->size != size || backend->content_revision != revision ||
        backend->fat_file != file || backend->detached || backend->data != 0 ||
        backend->unlink_prepared) {
        vfs_backend_unlock(backend);
        kfree(snapshot);
        return K_EBUSY;
    }
    backend->data = snapshot;
    backend->data_capacity = size;
    backend->snapshot_size = size;
    backend->snapshot_revision = revision;
    backend->unlink_prepared = true;
    vfs_backend_unlock(backend);
    return K_OK;
}

void vfs_fat32_backend_commit_unlink_locked(vfs_backend_node_t *backend) {
    if (backend == 0) return;
    __atomic_store_n(&backend->detached, true, __ATOMIC_RELEASE);
    backend->unlink_prepared = false;
}

void vfs_fat32_backend_abort_unlink(vfs_backend_node_t *backend) {
    uint8_t *snapshot;
    if (backend == 0) return;
    vfs_backend_lock(backend);
    snapshot = backend->data;
    backend->data = 0;
    backend->data_capacity = 0;
    backend->unlink_prepared = false;
    vfs_backend_unlock(backend);
    kfree(snapshot);
}

kstatus_t vfs_register_backend_file_ex(const char *path, uint64_t size, uint32_t mode,
                                       vfs_backend_read_t read,
                                       vfs_backend_write_t write,
                                       vfs_backend_fsync_t fsync,
                                       vfs_backend_truncate_t truncate,
                                       void *context) {
    if (path == 0 || path[0] != '/' || read == 0 ||
        (mode & 0170000U) != 0100000U) return K_EINVAL;
    size_t path_length = vfs_string_length(path);
    if (path_length >= VFS_PATH_LIMIT) return K_EINVAL;
    vfs_backend_node_t *backend =
        (vfs_backend_node_t *)kzalloc(sizeof(*backend), 0);
    if (backend == 0) return K_ENOMEM;
    memcpy(backend->path, path, path_length + 1U);
    backend->size = size;
    backend->mode = mode;
    backend->directory = false;
    backend->mutable_node = false;
    backend->read = read;
    backend->write = write;
    backend->fsync = fsync;
    backend->truncate = truncate;
    backend->context = context;
    atomic_init(&backend->io_lock.state, 0U);
    return vfs_register_node(backend);
}

kstatus_t vfs_register_backend_file(const char *path, uint64_t size, uint32_t mode,
                                    vfs_backend_read_t read,
                                    vfs_backend_write_t write,
                                    vfs_backend_fsync_t fsync,
                                    void *context) {
    return vfs_register_backend_file_ex(path, size, mode, read, write, fsync,
                                         0, context);
}

#endif
