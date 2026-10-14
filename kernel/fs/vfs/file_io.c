#include <arch/x86_64/uaccess.h>
#include <kernel/vfs.h>
#include "internal.h"

#ifdef OS_KERNEL_BUILD

/* REFACTOR_FS_VFS_FILE_IO_OWNER: kernel file position and data I/O policy. */

/* Direct I/O writes update the page-cache view so mmap observes new data. */
kstatus_t vfs_truncate_node(vnode_t *vnode, uint64_t size) {
    vfs_backend_node_t *backend;
    kstatus_t status;
    if (vnode == 0 || vnode->object.type != KOBJECT_TYPE_VNODE) return K_EINVAL;
    backend = (vfs_backend_node_t *)vnode->fs_private;
    if (backend == 0 || backend->directory) return K_EISDIR;
    if (backend->truncate == 0) return K_EACCES;
    vfs_vnode_lock(vnode);
    status = vfs_page_cache_flush(vnode);
    vfs_vnode_unlock(vnode);
    if (status != K_OK) return status;
    vfs_backend_lock(backend);
    status = backend->truncate(backend->context, size);
    vfs_backend_unlock(backend);
    if (status == K_OK) {
        vfs_page_cache_resize(vnode, size);
    }
    return status;
}

kstatus_t vfs_seek(file_t *file, int64_t offset, uint32_t whence,
                   uint64_t *position) {
    uint64_t base;
    uint64_t result;
    if (file == 0 || file->object.type != KOBJECT_TYPE_FILE ||
        file->vnode == 0 || position == 0 || file->vnode->fs_private == 0) {
        return K_EBADF;
    }
    vfs_backend_node_t *backend = (vfs_backend_node_t *)file->vnode->fs_private;
    if (backend->directory) return K_EISDIR;
    if (whence == OS_FILE_SEEK_SET) base = 0U;
    else if (whence == OS_FILE_SEEK_CURRENT) base = file->position;
    else if (whence == OS_FILE_SEEK_END) base = file->vnode->size;
    else return K_EINVAL;
    if (offset < 0) {
        uint64_t magnitude = (uint64_t)(-(offset + 1)) + 1U;
        if (magnitude > base) return K_EINVAL;
        result = base - magnitude;
    } else {
        if ((uint64_t)offset > UINT64_MAX - base) return K_EOVERFLOW;
        result = base + (uint64_t)offset;
    }
    file->position = result;
    *position = result;
    return K_OK;
}

kstatus_t vfs_truncate_kernel(file_t *file, uint64_t size) {
    if (file == 0 || file->object.type != KOBJECT_TYPE_FILE ||
        (file->rights & VFS_OPEN_WRITE) == 0U) return K_EBADF;
    return vfs_truncate_node(file->vnode, size);
}

kstatus_t vfs_truncate(file_t *file, uint64_t size) {
    return vfs_truncate_kernel(file, size);
}

/* Both read APIs share backend locking, EOF revalidation and count bounds.
 * Revalidate each chunk: a concurrent truncate may move EOF after progress. */
static kstatus_t vfs_read_backend_chunk(vfs_backend_node_t *backend,
                                       uint64_t position, void *buffer,
                                       size_t length, uint64_t *bytes) {
    *bytes = 0U;
    vfs_backend_lock(backend);
    kstatus_t status = position >= backend->size ? K_OK :
        backend->read(backend->context, position, buffer, length, bytes);
    vfs_backend_unlock(backend);
    return status == K_OK && *bytes > length ? K_EIO : status;
}

kstatus_t vfs_read(file_t *file, void __user *buf, size_t len, uint64_t *bytes) {
    if (file == 0 || file->object.type != KOBJECT_TYPE_FILE || file->vnode == 0 ||
        bytes == 0 || (file->rights & VFS_OPEN_READ) == 0U) return K_EINVAL;
    if (len != 0 && !x86_user_range_valid(buf, len)) return K_EINVAL;
    vfs_backend_node_t *backend =
        (vfs_backend_node_t *)file->vnode->fs_private;
    if (backend == 0 || backend->read == 0) return K_EIO;
    /* POSIX read() returns EOF, not EIO, when the file offset is at or past
     * the end of the file.  SQLite relies on this while probing/truncating
     * journal and database pages. */
    *bytes = 0U;
    if (file->position >= backend->size || len == 0U) return K_OK;
    size_t total = 0;
    kstatus_t result = K_OK;
    while (total < len) {
        uint8_t temporary[PAGE_SIZE];
        size_t chunk = len - total;
        if (chunk > sizeof(temporary)) chunk = sizeof(temporary);
        uint64_t got = 0;
        kstatus_t status = vfs_read_backend_chunk(backend, file->position,
                                                  temporary, chunk, &got);
        if (status != K_OK) {
            result = status;
            break;
        }
        if (got == 0) break;
        if (copy_to_user((uint8_t __user *)buf + total, temporary, (size_t)got) != K_OK) {
            result = K_EACCES;
            break;
        }
        total += (size_t)got;
        file->position += got;
        if (got < chunk) break;
    }
    *bytes = total;
    return result;
}

kstatus_t vfs_read_kernel(file_t *file, void *buf, size_t len, uint64_t *bytes) {
    if (file == 0 || file->object.type != KOBJECT_TYPE_FILE || file->vnode == 0 ||
        buf == 0 || bytes == 0 || (file->rights & VFS_OPEN_READ) == 0U) return K_EINVAL;
    vfs_backend_node_t *backend =
        (vfs_backend_node_t *)file->vnode->fs_private;
    if (backend == 0 || backend->read == 0) return K_EIO;
    /* Keep the kernel-side API identical to the user read path at EOF. */
    *bytes = 0U;
    if (file->position >= backend->size || len == 0U) return K_OK;
    size_t total = 0;
    kstatus_t result = K_OK;
    while (total < len) {
        size_t chunk = len - total;
        if (chunk > UINT32_MAX) chunk = UINT32_MAX;
        uint64_t got = 0;
        kstatus_t status = vfs_read_backend_chunk(backend, file->position,
                                                  (uint8_t *)buf + total, chunk, &got);
        if (status != K_OK) {
            result = status;
            break;
        }
        if (got == 0) break;
        total += (size_t)got;
        file->position += got;
        if (got < chunk) break;
    }
    *bytes = total;
    return result;
}

/* Commit only a validated backend count; cache copies must not hold its lock. */
static kstatus_t vfs_write_chunk(file_t *file, vfs_backend_node_t *backend,
                                const void *buffer, size_t length,
                                uint64_t *bytes) {
    *bytes = 0U;
    vfs_backend_lock(backend);
    kstatus_t status = backend->write(backend->context, file->position,
                                      buffer, length, bytes);
    vfs_backend_unlock(backend);
    if (status != K_OK || *bytes > length) return status == K_OK ? K_EIO : status;
    if (*bytes != 0U) {
        vfs_page_cache_copy_in(file->vnode, file->position, buffer, (size_t)*bytes);
        file->position += *bytes;
    }
    return K_OK;
}

kstatus_t vfs_write(file_t *file, const void __user *buf, size_t len, uint64_t *bytes) {
    if (file == 0 || file->object.type != KOBJECT_TYPE_FILE || file->vnode == 0 ||
        bytes == 0 || (file->rights & VFS_OPEN_WRITE) == 0U) return K_EACCES;
    vfs_backend_node_t *backend =
        (vfs_backend_node_t *)file->vnode->fs_private;
    if (backend == 0 || backend->write == 0) return K_EACCES;
    if (len != 0 && !x86_user_range_valid(buf, len)) return K_EINVAL;
    *bytes = 0U;
    size_t total = 0;
    kstatus_t result = K_OK;
    while (total < len) {
        uint8_t temporary[PAGE_SIZE];
        size_t chunk = len - total;
        if (chunk > sizeof(temporary)) chunk = sizeof(temporary);
        if (copy_from_user(temporary, (const uint8_t __user *)buf + total, chunk) != K_OK) {
            result = K_EACCES;
            break;
        }
        uint64_t written = 0;
        kstatus_t status = vfs_write_chunk(file, backend, temporary, chunk, &written);
        if (status != K_OK) {
            result = status;
            break;
        }
        if (written == 0) break;
        total += (size_t)written;
        if (written < chunk) break;
    }
    *bytes = total;
    return result;
}

kstatus_t vfs_write_kernel(file_t *file, const void *buf, size_t len,
                           uint64_t *bytes) {
    if (file == 0 || file->object.type != KOBJECT_TYPE_FILE || file->vnode == 0 ||
        (buf == 0 && len != 0) || bytes == 0 ||
        (file->rights & VFS_OPEN_WRITE) == 0U) return K_EACCES;
    vfs_backend_node_t *backend =
        (vfs_backend_node_t *)file->vnode->fs_private;
    if (backend == 0 || backend->write == 0) return K_EACCES;
    *bytes = 0U;
    size_t total = 0;
    kstatus_t result = K_OK;
    while (total < len) {
        size_t chunk = len - total;
        if (chunk > PAGE_SIZE) chunk = PAGE_SIZE;
        uint64_t written = 0;
        kstatus_t status = vfs_write_chunk(file, backend,
                                          (const uint8_t *)buf + total, chunk, &written);
        if (status != K_OK) {
            result = status;
            break;
        }
        if (written == 0) break;
        total += (size_t)written;
        if (written < chunk) break;
    }
    *bytes = total;
    return result;
}

kstatus_t vfs_fsync(file_t *file) {
    if (file == 0 || file->object.type != KOBJECT_TYPE_FILE || file->vnode == 0) {
        return K_EINVAL;
    }
    vfs_backend_node_t *backend =
        (vfs_backend_node_t *)file->vnode->fs_private;
    if (backend == 0) return K_EIO;
    return vfs_page_cache_sync(file->vnode, 0U, UINT64_MAX);
}

void vfs_close(file_t *file) {
    object_put(file);
}

#endif
