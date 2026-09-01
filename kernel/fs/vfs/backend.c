#include <kernel/kmem.h>
#include <kernel/console.h>
#include "internal.h"

#ifdef LITEOS_KERNEL_BUILD

/* REFACTOR_FS_VFS_BACKEND_OWNER: memory/FAT32 adapters and backend registration. */

kstatus_t vfs_memory_backend_read(void *context, uint64_t offset,
                                   void *buffer, size_t length,
                                   uint64_t *bytes) {
    vfs_backend_node_t *backend = (vfs_backend_node_t *)context;
    if (backend == 0 || bytes == 0 || (length != 0 && buffer == 0) ||
        offset > backend->size) return K_EINVAL;
    uint64_t count = backend->size - offset;
    if (count > length) count = length;
    for (uint64_t i = 0; i < count; ++i) {
        ((uint8_t *)buffer)[i] = backend->data[offset + i];
    }
    *bytes = count;
    return K_OK;
}

kstatus_t vfs_memory_backend_write(void *context, uint64_t offset,
                                    const void *buffer, size_t length,
                                    uint64_t *bytes) {
    vfs_backend_node_t *backend = (vfs_backend_node_t *)context;
    uint64_t required;
    uint8_t *replacement;
    if (backend == 0 || backend->directory || bytes == 0 ||
        (length != 0U && buffer == 0) || offset > UINT64_MAX - length) {
        return K_EINVAL;
    }
    required = offset + length;
    if (required > backend->size) {
        if (required > SIZE_MAX) return K_EOVERFLOW;
        replacement = (uint8_t *)kmalloc((size_t)required, 0);
        if (replacement == 0 && required != 0U) return K_ENOMEM;
        for (uint64_t index = 0U; index < required; ++index) {
            replacement[index] = index < backend->size ? backend->data[index] : 0U;
        }
        kfree(backend->data);
        backend->data = replacement;
        backend->size = required;
    }
    for (size_t index = 0U; index < length; ++index) {
        backend->data[offset + index] = ((const uint8_t *)buffer)[index];
    }
    *bytes = length;
    return K_OK;
}

kstatus_t vfs_memory_backend_truncate(void *context, uint64_t size) {
    vfs_backend_node_t *backend = (vfs_backend_node_t *)context;
    uint8_t *replacement;
    if (backend == 0 || backend->directory || size > SIZE_MAX) return K_EINVAL;
    if (size == backend->size) return K_OK;
    if (size == 0U) {
        kfree(backend->data);
        backend->data = 0;
        backend->size = 0U;
        return K_OK;
    }
    replacement = (uint8_t *)kmalloc((size_t)size, 0);
    if (replacement == 0) return K_ENOMEM;
    for (uint64_t index = 0U; index < size; ++index) {
        replacement[index] = index < backend->size ? backend->data[index] : 0U;
    }
    kfree(backend->data);
    backend->data = replacement;
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
    if (backend == 0 || bytes == 0 || length > UINT32_MAX ||
        (mount = vfs_fat32_mount_for_path(backend->path, relative)) == 0 ||
        relative[0] == 0 ||
        !liteos_fat32_read_path(mount->filesystem, relative, offset,
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
    if (backend == 0 || bytes == 0 || length > UINT32_MAX ||
        (mount = vfs_fat32_mount_for_path(backend->path, relative)) == 0 ||
        relative[0] == 0 ||
        !liteos_fat32_write_path(mount->filesystem, relative, offset,
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
    if (backend == 0 ||
        (mount = vfs_fat32_mount_for_path(backend->path, relative)) == 0) {
        return K_EIO;
    }
    return liteos_fat32_sync(mount->filesystem) ? K_OK : K_EIO;
}

kstatus_t vfs_fat32_backend_truncate(void *context, uint64_t size) {
    vfs_backend_node_t *backend = (vfs_backend_node_t *)context;
    char relative[VFS_PATH_LIMIT];
    vfs_fat32_mount_t *mount;
    if (backend != 0 && backend->detached) {
        return vfs_memory_backend_truncate(backend, size);
    }
    if (backend == 0 ||
        (mount = vfs_fat32_mount_for_path(backend->path, relative)) == 0 ||
        relative[0] == 0 || !liteos_fat32_truncate_path(mount->filesystem,
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
    vfs_fat32_mount_t *mount;
    if (backend == 0 || backend->directory || backend->detached) return K_OK;
    if ((mount = vfs_fat32_mount_for_path(backend->path, relative)) == 0 ||
        relative[0] == 0U ||
        backend->size > SIZE_MAX) return K_EINVAL;
    if (backend->size != 0U) {
        snapshot = (uint8_t *)kmalloc((size_t)backend->size, 0);
        if (snapshot == 0) return K_ENOMEM;
        while (offset < backend->size) {
            uint64_t remaining = backend->size - offset;
            UINT32 capacity = remaining > UINT32_MAX ? UINT32_MAX :
                              (UINT32)remaining;
            UINT32 transferred = 0U;
            if (!liteos_fat32_read_path(mount->filesystem, relative,
                                        offset, snapshot + offset, capacity,
                                        &transferred) ||
                transferred != capacity) {
                kfree(snapshot);
                return K_EIO;
            }
            offset += transferred;
        }
    }
    vfs_backend_lock(backend);
    backend->data = snapshot;
    vfs_backend_unlock(backend);
    return K_OK;
}

void vfs_fat32_backend_commit_unlink(vfs_backend_node_t *backend) {
    if (backend == 0) return;
    vfs_backend_lock(backend);
    backend->detached = true;
    vfs_backend_unlock(backend);
}

void vfs_fat32_backend_abort_unlink(vfs_backend_node_t *backend) {
    uint8_t *snapshot;
    if (backend == 0) return;
    vfs_backend_lock(backend);
    snapshot = backend->data;
    backend->data = 0;
    vfs_backend_unlock(backend);
    kfree(snapshot);
}

kstatus_t vfs_register_backend_file_ex(const char *path, uint64_t size, uint32_t mode,
                                       vfs_backend_read_t read,
                                       vfs_backend_write_t write,
                                       vfs_backend_fsync_t fsync,
                                       vfs_backend_truncate_t truncate,
                                       void *context) {
    if (path == 0 || path[0] != '/' || vfs_string_length(path) == 0 ||
        vfs_string_length(path) >= VFS_PATH_LIMIT || read == 0 ||
        (mode & 0170000U) != 0100000U) return K_EINVAL;
    vfs_backend_node_t *backend =
        (vfs_backend_node_t *)kzalloc(sizeof(*backend), 0);
    if (backend == 0) return K_ENOMEM;
    size_t path_length = vfs_string_length(path);
    for (size_t i = 0; i <= path_length; ++i) backend->path[i] = path[i];
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
