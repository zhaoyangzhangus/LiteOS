#include <arch/x86_64/uaccess.h>
#include <kernel/kmem.h>
#include <kernel/object.h>
#include <kernel/vfs.h>
#include "fat32.h"
#include "vfs.h"

static const LITEOS_VFS_FILE_OPERATIONS g_ramfs_operations;

#ifdef LITEOS_KERNEL_BUILD
#define VFS_MEMORY_NODE_LIMIT 64U
#define VFS_PATH_LIMIT         256U

typedef struct vfs_backend_node {
    char path[VFS_PATH_LIMIT];
    uint8_t *data;
    uint64_t size;
    uint32_t mode;
    bool directory;
    bool mutable_node;
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

typedef struct {
    vfs_cached_page_t *head;
} vfs_page_cache_t;

typedef struct {
    char prefix[VFS_PATH_LIMIT];
    LITEOS_FAT32 *filesystem;
    bool mounted;
} vfs_fat32_mount_t;

static vnode_t *g_vfs_nodes[VFS_MEMORY_NODE_LIMIT];
static spinlock_t g_vfs_lock;
static atomic_uint g_vfs_init_state;
static vfs_fat32_mount_t g_fat32_mount;

static void vfs_lock(void) {
    while (atomic_exchange_explicit(&g_vfs_lock.state, 1U,
                                    memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
}

static void vfs_unlock(void) {
    atomic_store_explicit(&g_vfs_lock.state, 0U, memory_order_release);
}

static void backend_lock(vfs_backend_node_t *backend) {
    while (atomic_exchange_explicit(&backend->io_lock.state, 1U,
                                    memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
}

static void backend_unlock(vfs_backend_node_t *backend) {
    atomic_store_explicit(&backend->io_lock.state, 0U, memory_order_release);
}

static void vnode_lock(vnode_t *vnode) {
    while (atomic_exchange_explicit(&vnode->lock.state, 1U,
                                    memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
}

static void vnode_unlock(vnode_t *vnode) {
    atomic_store_explicit(&vnode->lock.state, 0U, memory_order_release);
}

static kstatus_t vnode_page_cache_flush(vnode_t *vnode);

static void vnode_page_cache_destroy(vnode_t *vnode) {
    vfs_page_cache_t *cache = vnode != 0 ?
                              (vfs_page_cache_t *)vnode->page_cache : 0;
    if (cache == 0) return;
    vfs_cached_page_t *entry = cache->head;
    while (entry != 0) {
        vfs_cached_page_t *next = entry->next;
        page_free(entry->page);
        kfree(entry);
        entry = next;
    }
    kfree(cache);
    vnode->page_cache = 0;
}

static kstatus_t vnode_page_cache_flush(vnode_t *vnode) {
    if (vnode == 0) return K_EINVAL;
    vfs_backend_node_t *backend = (vfs_backend_node_t *)vnode->fs_private;
    vfs_page_cache_t *cache = (vfs_page_cache_t *)vnode->page_cache;
    if (backend == 0 || cache == 0) return K_OK;
    for (vfs_cached_page_t *entry = cache->head; entry != 0; entry = entry->next) {
        if (!entry->dirty) continue;
        if (entry->index > UINT64_MAX / PAGE_SIZE) return K_EINVAL;
        uint64_t offset = entry->index * PAGE_SIZE;
        if (offset >= vnode->size) {
            entry->dirty = false;
            continue;
        }
        uint64_t length = vnode->size - offset;
        if (length > PAGE_SIZE) length = PAGE_SIZE;
        uint64_t written = 0;
        backend_lock(backend);
        kstatus_t status = backend->write == 0 ? K_EACCES :
            backend->write(backend->context, offset,
                           phys_to_direct(page_to_phys(entry->page)),
                           (size_t)length, &written);
        backend_unlock(backend);
        if (status != K_OK || written != length) return status == K_OK ? K_EIO : status;
        entry->dirty = false;
    }
    return K_OK;
}

static size_t vfs_string_length(const char *text) {
    size_t length = 0;
    if (text == 0) return 0;
    while (length < VFS_PATH_LIMIT && text[length] != 0) ++length;
    return length;
}

static bool vfs_copy_path_from_user(const char __user *source,
                                    char destination[VFS_PATH_LIMIT]) {
    if (source == 0) return false;
    for (size_t i = 0; i + 1U < VFS_PATH_LIMIT; ++i) {
        char value = 0;
        if (copy_from_user(&value, source + i, sizeof(value)) != K_OK) return false;
        destination[i] = value;
        if (value == 0) return i != 0 && destination[0] == '/';
    }
    destination[VFS_PATH_LIMIT - 1U] = 0;
    return false;
}

static void vnode_destroy(void *object) {
    vnode_t *vnode = (vnode_t *)object;
    vfs_backend_node_t *backend = vnode != 0 ?
                                  (vfs_backend_node_t *)vnode->fs_private : 0;
    if (vnode != 0) {
        /* 最后一个 vnode 引用释放时仍要尽力写回 mmap 脏页。 */
        vnode_lock(vnode);
        (void)vnode_page_cache_flush(vnode);
        vnode_unlock(vnode);
        vnode_page_cache_destroy(vnode);
    }
    if (backend != 0) {
        kfree(backend->data);
        kfree(backend);
    }
    kfree(vnode);
}

static void file_destroy(void *object) {
    file_t *file = (file_t *)object;
    if (file != 0) {
        if (file->vnode != 0) object_put(file->vnode);
        kfree(file);
    }
}

static const object_ops_t g_vnode_object_ops = {
    .destroy = vnode_destroy,
    .type_name = "Vnode",
    .is_signaled = 0,
    .wait_value = 0,
};

static const object_ops_t g_file_object_ops = {
    .destroy = file_destroy,
    .type_name = "File",
    .is_signaled = 0,
    .wait_value = 0,
};

static void vfs_initialize(void) {
    unsigned expected = 0U;
    if (atomic_compare_exchange_strong_explicit(&g_vfs_init_state, &expected, 1U,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        atomic_init(&g_vfs_lock.state, 0U);
        for (uint32_t i = 0; i < VFS_MEMORY_NODE_LIMIT; ++i) g_vfs_nodes[i] = 0;
        atomic_store_explicit(&g_vfs_init_state, 2U, memory_order_release);
        return;
    }
    while (atomic_load_explicit(&g_vfs_init_state, memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
}

static vnode_t *vfs_find_node_locked(const char *path) {
    for (uint32_t i = 0; i < VFS_MEMORY_NODE_LIMIT; ++i) {
        vnode_t *vnode = g_vfs_nodes[i];
        vfs_backend_node_t *backend = vnode != 0 ?
                                      (vfs_backend_node_t *)vnode->fs_private : 0;
        if (backend == 0) continue;
        size_t path_length = vfs_string_length(path);
        if (path_length == vfs_string_length(backend->path)) {
            size_t j = 0;
            while (j < path_length && path[j] == backend->path[j]) ++j;
            if (j == path_length) return vnode;
        }
    }
    return 0;
}

static bool vfs_path_range_equals(const char *left, size_t left_length,
                                  const char *right, size_t right_length) {
    if (left == 0 || right == 0 || left_length != right_length) return false;
    for (size_t index = 0; index < left_length; ++index) {
        if (left[index] != right[index]) return false;
    }
    return true;
}

static bool vfs_fat32_relative_path(const char *path, char relative[VFS_PATH_LIMIT]) {
    size_t path_length;
    size_t prefix_length;
    if (!g_fat32_mount.mounted || g_fat32_mount.filesystem == 0 ||
        path == 0 || relative == 0 || path[0] != '/') return false;
    path_length = vfs_string_length(path);
    prefix_length = vfs_string_length(g_fat32_mount.prefix);
    if (prefix_length == 0U || path_length < prefix_length ||
        !vfs_path_range_equals(path, prefix_length,
                               g_fat32_mount.prefix, prefix_length)) return false;
    if (path_length == prefix_length) {
        relative[0] = 0;
        return true;
    }
    if (prefix_length == 1U && g_fat32_mount.prefix[0] == '/') {
        size_t relative_length = path_length - 1U;
        if (relative_length >= VFS_PATH_LIMIT) return false;
        for (size_t index = 0U; index < relative_length; ++index) {
            relative[index] = path[index + 1U];
        }
        relative[relative_length] = 0;
        return true;
    }
    if (path[prefix_length] != '/') return false;
    size_t relative_length = path_length - prefix_length - 1U;
    if (relative_length >= VFS_PATH_LIMIT) return false;
    for (size_t index = 0U; index < relative_length; ++index) {
        relative[index] = path[prefix_length + 1U + index];
    }
    relative[relative_length] = 0;
    return true;
}

static bool vfs_fat32_stat_absolute(const char *path, os_file_info_t *info,
                                    char relative[VFS_PATH_LIMIT]) {
    if (!vfs_fat32_relative_path(path, relative)) return false;
    return liteos_fat32_stat_path(g_fat32_mount.filesystem, relative, info);
}

kstatus_t vfs_mount_fat32(const char *prefix, struct LITEOS_FAT32 *filesystem) {
    size_t length;
    if (prefix == 0 || filesystem == 0 || !filesystem->Mounted || prefix[0] != '/') {
        return K_EINVAL;
    }
    length = vfs_string_length(prefix);
    if (length == 0U || length >= VFS_PATH_LIMIT ||
        (length > 1U && prefix[length - 1U] == '/')) {
        return K_EINVAL;
    }
    if (g_fat32_mount.mounted) return K_EBUSY;
    for (size_t index = 0U; index <= length; ++index) {
        g_fat32_mount.prefix[index] = prefix[index];
    }
    g_fat32_mount.filesystem = filesystem;
    g_fat32_mount.mounted = true;
    return K_OK;
}

kstatus_t vfs_unmount_fat32(void) {
    if (!g_fat32_mount.mounted) return K_ENOENT;
    g_fat32_mount.prefix[0] = 0;
    g_fat32_mount.filesystem = 0;
    g_fat32_mount.mounted = false;
    return K_OK;
}

static bool vfs_directory_exists_locked(const char *path, size_t path_length) {
    if (path == 0 || path_length == 0U) return false;
    if (path_length == 1U && path[0] == '/') return true;
    char relative[VFS_PATH_LIMIT];
    os_file_info_t mounted_info;
    if (vfs_fat32_stat_absolute(path, &mounted_info, relative)) {
        return mounted_info.type == OS_FILE_TYPE_DIRECTORY;
    }
    for (uint32_t node_index = 0U; node_index < VFS_MEMORY_NODE_LIMIT; ++node_index) {
        vnode_t *vnode = g_vfs_nodes[node_index];
        vfs_backend_node_t *backend = vnode != 0 ?
                                      (vfs_backend_node_t *)vnode->fs_private : 0;
        size_t node_length;
        if (backend == 0) continue;
        node_length = vfs_string_length(backend->path);
        if (vfs_path_range_equals(backend->path, node_length, path, path_length)) {
            return backend->directory;
        }
        if (node_length > path_length && backend->path[path_length] == '/' &&
            vfs_path_range_equals(backend->path, path_length, path, path_length)) {
            return true;
        }
    }
    return false;
}

static bool vfs_parent_directory_exists_locked(const char *path, size_t path_length) {
    size_t slash = path_length;
    if (path == 0 || path_length <= 1U || path[path_length - 1U] == '/') return false;
    while (slash > 0U && path[slash - 1U] != '/') --slash;
    if (slash <= 1U) return true;
    return vfs_directory_exists_locked(path, slash - 1U);
}

static void vfs_set_leaf_name(const char *path, char name[OS_FILE_NAME_MAX]) {
    size_t length = vfs_string_length(path);
    size_t start = length;
    if (length == 1U && path[0] == '/') {
        name[0] = '/';
        name[1] = 0;
        return;
    }
    while (start > 0U && path[start - 1U] != '/') --start;
    size_t count = length - start;
    if (count >= OS_FILE_NAME_MAX) count = OS_FILE_NAME_MAX - 1U;
    for (size_t index = 0U; index < count; ++index) name[index] = path[start + index];
    name[count] = 0;
}

static void vfs_fill_info(const char *path, const vnode_t *vnode,
                          const vfs_backend_node_t *backend,
                          os_file_info_t *out) {
    *out = (os_file_info_t){0};
    out->type = backend != 0 && backend->directory ?
        OS_FILE_TYPE_DIRECTORY : OS_FILE_TYPE_REGULAR;
    out->mode = backend != 0 ? backend->mode : 0040555U;
    out->size = vnode != 0 && backend != 0 && !backend->directory ? vnode->size : 0U;
    vfs_set_leaf_name(path, out->name);
}

/* The current VFS stores files by absolute path.  Derive directory entries
 * from those paths here so every user of the VFS sees the same namespace. */
kstatus_t vfs_enumerate_kernel(const char *path, uint32_t index,
                               os_file_info_t *out) {
    os_file_info_t entries[VFS_MEMORY_NODE_LIMIT];
    size_t directory_length;
    uint32_t entry_count = 0U;
    bool directory_exists = false;
    bool exact_regular_file = false;

    if (path == 0 || out == 0 || path[0] != '/') return K_EINVAL;
    directory_length = vfs_string_length(path);
    if (directory_length == 0U || directory_length >= VFS_PATH_LIMIT) return K_EINVAL;
    while (directory_length > 1U && path[directory_length - 1U] == '/') --directory_length;
    if (g_fat32_mount.mounted) {
        char relative[VFS_PATH_LIMIT];
        os_file_info_t mounted_info;
        if (vfs_fat32_stat_absolute(path, &mounted_info, relative)) {
            if (mounted_info.type != OS_FILE_TYPE_DIRECTORY) return K_EINVAL;
            return liteos_fat32_enumerate_path(g_fat32_mount.filesystem, relative,
                                               index, out) ? K_OK : K_ENOENT;
        }
    }
    for (uint32_t entry_index = 0U; entry_index < VFS_MEMORY_NODE_LIMIT; ++entry_index) {
        entries[entry_index] = (os_file_info_t){0};
    }
    if (directory_length == 1U) directory_exists = true;
    if (directory_length == 1U && g_fat32_mount.mounted) {
        const char *mount_name = g_fat32_mount.prefix + 1U;
        size_t mount_name_length = 0U;
        while (mount_name[mount_name_length] != 0 && mount_name[mount_name_length] != '/') {
            ++mount_name_length;
        }
        if (mount_name_length != 0U && mount_name_length < OS_FILE_NAME_MAX &&
            entry_count < VFS_MEMORY_NODE_LIMIT) {
            entries[entry_count] = (os_file_info_t){0};
            entries[entry_count].type = OS_FILE_TYPE_DIRECTORY;
            entries[entry_count].mode = 0040755U;
            for (size_t index = 0U; index < mount_name_length; ++index) {
                entries[entry_count].name[index] = mount_name[index];
            }
            entries[entry_count].name[mount_name_length] = 0;
            ++entry_count;
        }
    }

    vfs_initialize();
    vfs_lock();
    for (uint32_t node_index = 0U; node_index < VFS_MEMORY_NODE_LIMIT; ++node_index) {
        vnode_t *vnode = g_vfs_nodes[node_index];
        vfs_backend_node_t *backend = vnode != 0 ?
                                      (vfs_backend_node_t *)vnode->fs_private : 0;
        size_t node_length;
        size_t remainder_index;
        const char *remainder;
        if (backend == 0) continue;
        node_length = vfs_string_length(backend->path);
        if (vfs_path_range_equals(backend->path, node_length, path, directory_length)) {
            if (backend->directory) directory_exists = true;
            else exact_regular_file = true;
            continue;
        }
        if (directory_length == 1U) {
            if (node_length <= 1U || backend->path[0] != '/') continue;
            remainder_index = 1U;
        } else {
            if (node_length <= directory_length ||
                backend->path[directory_length] != '/' ||
                !vfs_path_range_equals(backend->path, directory_length,
                                       path, directory_length)) continue;
            remainder_index = directory_length + 1U;
        }
        if (remainder_index >= node_length) continue;
        directory_exists = true;
        remainder = backend->path + remainder_index;
        size_t name_length = 0U;
        while (remainder[name_length] != '\0' && remainder[name_length] != '/') ++name_length;
        if (name_length == 0U || name_length >= OS_FILE_NAME_MAX) {
            vfs_unlock();
            return K_EINVAL;
        }
        bool child_directory = backend->directory || remainder[name_length] == '/';
        uint32_t found = entry_count;
        for (uint32_t existing = 0U; existing < entry_count; ++existing) {
            if (vfs_path_range_equals(entries[existing].name,
                                      vfs_string_length(entries[existing].name),
                                      remainder, name_length)) {
                found = existing;
                break;
            }
        }
        if (found == entry_count) {
            if (entry_count >= VFS_MEMORY_NODE_LIMIT) {
                vfs_unlock();
                return K_ENOMEM;
            }
            for (size_t character = 0U; character < name_length; ++character) {
                entries[entry_count].name[character] = remainder[character];
            }
            entries[entry_count].name[name_length] = '\0';
            entries[entry_count].type = child_directory ?
                OS_FILE_TYPE_DIRECTORY : OS_FILE_TYPE_REGULAR;
            entries[entry_count].mode = child_directory ?
                (backend->directory ? vnode->mode : 0040555U) : vnode->mode;
            entries[entry_count].size = child_directory ? 0U : vnode->size;
            ++entry_count;
        } else if (child_directory) {
            entries[found].type = OS_FILE_TYPE_DIRECTORY;
            entries[found].mode = 0040555U;
            entries[found].size = 0U;
        }
    }
    vfs_unlock();
    if (!directory_exists) return exact_regular_file ? K_EINVAL : K_ENOENT;
    if (index >= entry_count) return K_ENOENT;
    *out = entries[index];
    return K_OK;
}

static kstatus_t memory_backend_read(void *context, uint64_t offset, void *buffer,
                                     size_t length, uint64_t *bytes) {
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

static kstatus_t memory_backend_write(void *context, uint64_t offset,
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

static kstatus_t memory_backend_truncate(void *context, uint64_t size) {
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

static kstatus_t fat32_backend_read(void *context, uint64_t offset, void *buffer,
                                    size_t length, uint64_t *bytes) {
    vfs_backend_node_t *backend = (vfs_backend_node_t *)context;
    char relative[VFS_PATH_LIMIT];
    UINT32 transferred = 0U;
    if (backend == 0 || bytes == 0 || length > UINT32_MAX ||
        !vfs_fat32_relative_path(backend->path, relative) || relative[0] == 0 ||
        !liteos_fat32_read_path(g_fat32_mount.filesystem, relative, offset,
                                buffer, (UINT32)length, &transferred)) return K_EIO;
    *bytes = transferred;
    return K_OK;
}

static kstatus_t fat32_backend_write(void *context, uint64_t offset,
                                     const void *buffer, size_t length,
                                     uint64_t *bytes) {
    vfs_backend_node_t *backend = (vfs_backend_node_t *)context;
    char relative[VFS_PATH_LIMIT];
    UINT32 transferred = 0U;
    if (backend == 0 || bytes == 0 || length > UINT32_MAX ||
        !vfs_fat32_relative_path(backend->path, relative) || relative[0] == 0 ||
        !liteos_fat32_write_path(g_fat32_mount.filesystem, relative, offset,
                                 buffer, (UINT32)length, &transferred)) return K_EIO;
    *bytes = transferred;
    if (offset <= UINT64_MAX - transferred && offset + transferred > backend->size) {
        backend->size = offset + transferred;
    }
    return K_OK;
}

static kstatus_t fat32_backend_fsync(void *context) {
    vfs_backend_node_t *backend = (vfs_backend_node_t *)context;
    char relative[VFS_PATH_LIMIT];
    if (backend == 0 || !vfs_fat32_relative_path(backend->path, relative)) return K_EIO;
    return liteos_fat32_sync(g_fat32_mount.filesystem) ? K_OK : K_EIO;
}

static kstatus_t fat32_backend_truncate(void *context, uint64_t size) {
    vfs_backend_node_t *backend = (vfs_backend_node_t *)context;
    char relative[VFS_PATH_LIMIT];
    if (backend == 0 || !vfs_fat32_relative_path(backend->path, relative) ||
        relative[0] == 0 || !liteos_fat32_truncate_path(g_fat32_mount.filesystem,
                                                        relative, size)) return K_EIO;
    backend->size = size;
    return K_OK;
}

static vnode_t *vfs_create_vnode_object(vfs_backend_node_t *backend) {
    vnode_t *vnode;
    if (backend == 0) return 0;
    vnode = (vnode_t *)kzalloc(sizeof(*vnode), 0);
    if (vnode == 0) return 0;
    refcount_init(&vnode->object.refs, 1U);
    vnode->object.type = KOBJECT_TYPE_VNODE;
    vnode->object.flags = 0;
    vnode->object.ops = &g_vnode_object_ops;
    vnode->object.security = 0;
    vnode->inode_id = 0;
    vnode->mode = backend->mode;
    vnode->flags = 0;
    vnode->size = backend->size;
    vnode->ops = 0;
    vnode->fs_private = backend;
    atomic_init(&vnode->lock.state, 0U);
    vnode->page_cache = 0;
    return vnode;
}

static kstatus_t vfs_create_fat32_vnode(const char *path, vnode_t **out) {
    os_file_info_t info;
    char relative[VFS_PATH_LIMIT];
    vfs_backend_node_t *backend;
    vnode_t *vnode;
    if (out == 0 || !vfs_fat32_stat_absolute(path, &info, relative) ||
        info.type == OS_FILE_TYPE_UNKNOWN) return K_ENOENT;
    backend = (vfs_backend_node_t *)kzalloc(sizeof(*backend), 0);
    if (backend == 0) return K_ENOMEM;
    size_t path_length = vfs_string_length(path);
    if (path_length >= VFS_PATH_LIMIT) {
        kfree(backend);
        return K_EINVAL;
    }
    for (size_t index = 0U; index <= path_length; ++index) backend->path[index] = path[index];
    backend->size = info.size;
    backend->mode = info.mode;
    backend->directory = info.type == OS_FILE_TYPE_DIRECTORY;
    backend->mutable_node = true;
    backend->read = backend->directory ? 0 : fat32_backend_read;
    backend->write = backend->directory ? 0 : fat32_backend_write;
    backend->fsync = backend->directory ? 0 : fat32_backend_fsync;
    backend->truncate = backend->directory ? 0 : fat32_backend_truncate;
    backend->context = backend;
    atomic_init(&backend->io_lock.state, 0U);
    vnode = vfs_create_vnode_object(backend);
    if (vnode == 0) {
        kfree(backend);
        return K_ENOMEM;
    }
    /* FAT32 路径也必须拥有规范 vnode，否则不同句柄各自维护页缓存，
     * mmap 与直接 I/O 会观察到不同内容。注册引用长期保留，调用者再持有
     * 一个引用，语义与内存后端节点一致。 */
    vfs_initialize();
    vfs_lock();
    vnode_t *existing = vfs_find_node_locked(path);
    if (existing != 0) {
        object_get(existing);
        vfs_unlock();
        object_put(vnode);
        *out = existing;
        return K_OK;
    }
    uint32_t slot = VFS_MEMORY_NODE_LIMIT;
    for (uint32_t index = 0U; index < VFS_MEMORY_NODE_LIMIT; ++index) {
        if (g_vfs_nodes[index] == 0) {
            slot = index;
            break;
        }
    }
    if (slot == VFS_MEMORY_NODE_LIMIT) {
        vfs_unlock();
        object_put(vnode);
        return K_ENOMEM;
    }
    g_vfs_nodes[slot] = vnode;
    object_get(vnode);
    vfs_unlock();
    *out = vnode;
    return K_OK;
}

static kstatus_t vfs_register_node(vfs_backend_node_t *backend) {
    if (backend == 0) return K_EINVAL;
    vfs_initialize();
    vnode_t *vnode = vfs_create_vnode_object(backend);
    if (vnode == 0) {
        kfree(backend->data);
        kfree(backend);
        return K_ENOMEM;
    }

    vfs_lock();
    if (vfs_find_node_locked(backend->path) != 0) {
        vfs_unlock();
        object_put(vnode);
        return K_EBUSY;
    }
    uint32_t slot = VFS_MEMORY_NODE_LIMIT;
    for (uint32_t i = 0; i < VFS_MEMORY_NODE_LIMIT; ++i) {
        if (g_vfs_nodes[i] == 0) {
            slot = i;
            break;
        }
    }
    if (slot == VFS_MEMORY_NODE_LIMIT) {
        vfs_unlock();
        object_put(vnode);
        return K_ENOMEM;
    }
    g_vfs_nodes[slot] = vnode;
    vfs_unlock();
    return K_OK;
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

static kstatus_t vfs_create_memory_node(const char *path, uint32_t mode,
                                        bool directory) {
    vfs_backend_node_t *backend;
    size_t path_length;
    char relative[VFS_PATH_LIMIT];
    os_file_info_t existing_info;
    bool exists;
    bool parent;
    if (path == 0 || path[0] != '/') return K_EINVAL;
    path_length = vfs_string_length(path);
    if (path_length <= 1U || path_length >= VFS_PATH_LIMIT ||
        path[path_length - 1U] == '/') return K_EINVAL;
    if (vfs_fat32_relative_path(path, relative)) {
        if (liteos_fat32_stat_path(g_fat32_mount.filesystem, relative, &existing_info)) {
            return K_EEXIST;
        }
        if (!liteos_fat32_create_path(g_fat32_mount.filesystem, relative, directory)) {
            return K_EIO;
        }
        return K_OK;
    }
    vfs_initialize();
    vfs_lock();
    exists = vfs_find_node_locked(path) != 0;
    parent = vfs_parent_directory_exists_locked(path, path_length);
    if (exists || !parent) {
        vfs_unlock();
        return exists ? K_EEXIST : (parent ? K_EBUSY : K_ENOTDIR);
    }
    vfs_unlock();

    backend = (vfs_backend_node_t *)kzalloc(sizeof(*backend), 0);
    if (backend == 0) return K_ENOMEM;
    for (size_t index = 0U; index <= path_length; ++index) {
        backend->path[index] = path[index];
    }
    backend->size = 0U;
    backend->mode = (directory ? 0040000U : 0100000U) |
                    (mode == 0U ? (directory ? 0755U : 0666U) : (mode & 0777U));
    backend->directory = directory;
    backend->mutable_node = true;
    backend->read = directory ? 0 : memory_backend_read;
    backend->write = directory ? 0 : memory_backend_write;
    backend->fsync = 0;
    backend->truncate = directory ? 0 : memory_backend_truncate;
    backend->context = backend;
    atomic_init(&backend->io_lock.state, 0U);
    kstatus_t status = vfs_register_node(backend);
    return status == K_EBUSY ? K_EEXIST : status;
}

kstatus_t vfs_create_kernel(const char *path, uint32_t mode, bool directory) {
    return vfs_create_memory_node(path, mode, directory);
}

kstatus_t vfs_mkdir_kernel(const char *path, uint32_t mode) {
    return vfs_create_memory_node(path, mode, true);
}

kstatus_t vfs_file_page_get(vnode_t *vnode, uint64_t page_index,
                            struct page **out) {
    if (vnode == 0 || vnode->object.type != KOBJECT_TYPE_VNODE || out == 0) {
        return K_EINVAL;
    }
    vfs_backend_node_t *backend = (vfs_backend_node_t *)vnode->fs_private;
    if (backend == 0 || backend->read == 0 ||
        page_index > UINT64_MAX / PAGE_SIZE) return K_EINVAL;
    vnode_lock(vnode);
    vfs_page_cache_t *cache = (vfs_page_cache_t *)vnode->page_cache;
    if (cache == 0) {
        cache = (vfs_page_cache_t *)kzalloc(sizeof(*cache), 0);
        if (cache == 0) {
            vnode_unlock(vnode);
            return K_ENOMEM;
        }
        cache->head = 0;
        vnode->page_cache = cache;
    }
    for (vfs_cached_page_t *entry = cache->head; entry != 0; entry = entry->next) {
        if (entry->index == page_index) {
            *out = entry->page;
            vnode_unlock(vnode);
            return K_OK;
        }
    }

    page_t *page = page_alloc(0, PAGE_ALLOC_ZERO);
    if (page == 0) {
        vnode_unlock(vnode);
        return K_ENOMEM;
    }
    page->owner = PAGE_OWNER_FILE;
    page->u.file.mapping = vnode;
    page->u.file.index = page_index;
    uint64_t file_offset = page_index * PAGE_SIZE;
    if (file_offset < backend->size) {
        uint64_t length = backend->size - file_offset;
        if (length > PAGE_SIZE) length = PAGE_SIZE;
        uint64_t bytes = 0;
        backend_lock(backend);
        kstatus_t status = backend->read(backend->context, file_offset,
                                         phys_to_direct(page_to_phys(page)),
                                         (size_t)length, &bytes);
        backend_unlock(backend);
        if (status != K_OK || bytes > length) {
            page_free(page);
            vnode_unlock(vnode);
            return status == K_OK ? K_EIO : status;
        }
    }
    vfs_cached_page_t *entry = (vfs_cached_page_t *)kzalloc(sizeof(*entry), 0);
    if (entry == 0) {
        page_free(page);
        vnode_unlock(vnode);
        return K_ENOMEM;
    }
    entry->index = page_index;
    entry->page = page;
    entry->dirty = false;
    entry->next = cache->head;
    cache->head = entry;
    *out = page;
    vnode_unlock(vnode);
    return K_OK;
}

void vfs_file_page_mark_dirty(vnode_t *vnode, uint64_t page_index) {
    if (vnode == 0) return;
    vnode_lock(vnode);
    vfs_page_cache_t *cache = (vfs_page_cache_t *)vnode->page_cache;
    if (cache != 0) {
        for (vfs_cached_page_t *entry = cache->head; entry != 0;
             entry = entry->next) {
            if (entry->index == page_index) {
                entry->dirty = true;
                break;
            }
        }
    }
    vnode_unlock(vnode);
}

/* 直接 I/O 写入后同步已经建立的文件页，保证 mmap 观察到最新内容。 */
static void vfs_cache_copy_in(vnode_t *vnode, uint64_t offset,
                              const uint8_t *source, size_t length) {
    if (vnode == 0 || source == 0 || length == 0) return;
    vnode_lock(vnode);
    vfs_page_cache_t *cache = (vfs_page_cache_t *)vnode->page_cache;
    size_t copied = 0;
    while (cache != 0 && copied < length) {
        uint64_t current = offset + copied;
        if (current < offset) break;
        uint64_t index = current / PAGE_SIZE;
        size_t in_page = (size_t)(current & (PAGE_SIZE - 1ULL));
        size_t chunk = PAGE_SIZE - in_page;
        if (chunk > length - copied) chunk = length - copied;
        for (vfs_cached_page_t *entry = cache->head; entry != 0;
             entry = entry->next) {
            if (entry->index != index) continue;
            uint8_t *page_memory = (uint8_t *)phys_to_direct(page_to_phys(entry->page));
            if (page_memory != 0) {
                for (size_t i = 0; i < chunk; ++i) {
                    page_memory[in_page + i] = source[copied + i];
                }
            }
            entry->dirty = false;
            break;
        }
        copied += chunk;
    }
    if (offset <= UINT64_MAX - length && offset + length > vnode->size) {
        vnode->size = offset + length;
    }
    vnode_unlock(vnode);
}

static kstatus_t vfs_truncate_node(vnode_t *vnode, uint64_t size) {
    vfs_backend_node_t *backend;
    kstatus_t status;
    if (vnode == 0 || vnode->object.type != KOBJECT_TYPE_VNODE) return K_EINVAL;
    backend = (vfs_backend_node_t *)vnode->fs_private;
    if (backend == 0 || backend->directory) return K_EISDIR;
    if (backend->truncate == 0) return K_EACCES;
    vnode_lock(vnode);
    status = vnode_page_cache_flush(vnode);
    vnode_unlock(vnode);
    if (status != K_OK) return status;
    backend_lock(backend);
    status = backend->truncate(backend->context, size);
    backend_unlock(backend);
    if (status == K_OK) {
        vnode_lock(vnode);
        vnode->size = size;
        vnode_unlock(vnode);
    }
    return status;
}

kstatus_t vfs_stat_kernel(const char *path, os_file_info_t *out) {
    size_t path_length;
    char relative[VFS_PATH_LIMIT];
    if (path == 0 || out == 0 || path[0] != '/') return K_EINVAL;
    path_length = vfs_string_length(path);
    if (path_length == 0U || path_length >= VFS_PATH_LIMIT) return K_EINVAL;
    while (path_length > 1U && path[path_length - 1U] == '/') --path_length;
    if (vfs_fat32_stat_absolute(path, out, relative)) return K_OK;
    vfs_initialize();
    vfs_lock();
    vnode_t *vnode = vfs_find_node_locked(path);
    if (vnode != 0) {
        vfs_backend_node_t *backend = (vfs_backend_node_t *)vnode->fs_private;
        if (backend == 0) {
            vfs_unlock();
            return K_EIO;
        }
        vfs_fill_info(path, vnode, backend, out);
        vfs_unlock();
        return K_OK;
    }
    if (vfs_directory_exists_locked(path, path_length)) {
        *out = (os_file_info_t){0};
        out->type = OS_FILE_TYPE_DIRECTORY;
        out->mode = 0040555U;
        vfs_set_leaf_name(path, out->name);
        vfs_unlock();
        return K_OK;
    }
    vfs_unlock();
    return K_ENOENT;
}

kstatus_t vfs_remove_kernel(const char *path) {
    size_t path_length;
    vnode_t *victim = 0;
    char relative[VFS_PATH_LIMIT];
    os_file_info_t mounted_info;
    if (path == 0 || path[0] != '/') return K_EINVAL;
    path_length = vfs_string_length(path);
    if (path_length <= 1U || path_length >= VFS_PATH_LIMIT) return K_EINVAL;
    while (path_length > 1U && path[path_length - 1U] == '/') --path_length;
    if (path_length <= 1U) return K_EINVAL;
    if (vfs_fat32_stat_absolute(path, &mounted_info, relative)) {
        vnode_t *mounted_victim = 0;
        if (!liteos_fat32_remove_path(g_fat32_mount.filesystem, relative)) {
            return mounted_info.type == OS_FILE_TYPE_DIRECTORY ? K_ENOTEMPTY : K_EIO;
        }
        /* 删除磁盘目录项后同步摘除规范 vnode；仍被打开的 file 引用会让
         * vnode 保持有效，但新的 open 不会再看到已经删除的路径。 */
        vfs_initialize();
        vfs_lock();
        for (uint32_t node_index = 0U; node_index < VFS_MEMORY_NODE_LIMIT; ++node_index) {
            vnode_t *candidate = g_vfs_nodes[node_index];
            vfs_backend_node_t *backend = candidate != 0 ?
                                          (vfs_backend_node_t *)candidate->fs_private : 0;
            size_t node_length;
            if (backend == 0) continue;
            node_length = vfs_string_length(backend->path);
            if (!vfs_path_range_equals(backend->path, node_length, path, path_length)) {
                continue;
            }
            g_vfs_nodes[node_index] = 0;
            mounted_victim = candidate;
            break;
        }
        vfs_unlock();
        if (mounted_victim != 0) object_put(mounted_victim);
        return K_OK;
    }
    vfs_initialize();
    vfs_lock();
    for (uint32_t node_index = 0U; node_index < VFS_MEMORY_NODE_LIMIT; ++node_index) {
        vnode_t *candidate = g_vfs_nodes[node_index];
        vfs_backend_node_t *backend = candidate != 0 ?
                                      (vfs_backend_node_t *)candidate->fs_private : 0;
        size_t node_length;
        if (backend == 0) continue;
        node_length = vfs_string_length(backend->path);
        if (!vfs_path_range_equals(backend->path, node_length, path, path_length)) continue;
        if (!backend->mutable_node) {
            vfs_unlock();
            return K_EACCES;
        }
        if (backend->directory) {
            for (uint32_t child_index = 0U; child_index < VFS_MEMORY_NODE_LIMIT;
                 ++child_index) {
                vnode_t *child = g_vfs_nodes[child_index];
                vfs_backend_node_t *child_backend = child != 0 ?
                    (vfs_backend_node_t *)child->fs_private : 0;
                size_t child_length;
                if (child_backend == 0 || child == candidate) continue;
                child_length = vfs_string_length(child_backend->path);
                if (child_length > path_length && child_backend->path[path_length] == '/' &&
                    vfs_path_range_equals(child_backend->path, path_length,
                                          path, path_length)) {
                    vfs_unlock();
                    return K_ENOTEMPTY;
                }
            }
        }
        g_vfs_nodes[node_index] = 0;
        victim = candidate;
        break;
    }
    vfs_unlock();
    if (victim == 0) return K_ENOENT;
    object_put(victim);
    return K_OK;
}

kstatus_t vfs_open_kernel(const char *path, uint32_t flags, file_t **out) {
    const uint32_t known_flags = VFS_OPEN_READ | VFS_OPEN_WRITE |
                                 VFS_OPEN_CREATE | VFS_OPEN_EXCLUSIVE |
                                 VFS_OPEN_TRUNCATE | VFS_OPEN_APPEND |
                                 VFS_OPEN_DIRECTORY;
    bool existed = false;
    if (path == 0 || out == 0 || path[0] != '/' ||
        (flags & ~known_flags) != 0U ||
        (flags & (VFS_OPEN_READ | VFS_OPEN_WRITE)) == 0U ||
        ((flags & VFS_OPEN_TRUNCATE) != 0U && (flags & VFS_OPEN_WRITE) == 0U) ||
        ((flags & VFS_OPEN_APPEND) != 0U && (flags & VFS_OPEN_DIRECTORY) != 0U)) {
        return K_EINVAL;
    }
    vfs_initialize();
    vnode_t *vnode = 0;
    os_file_info_t mounted_info;
    char relative[VFS_PATH_LIMIT];
    if (vfs_fat32_stat_absolute(path, &mounted_info, relative)) {
        kstatus_t mount_status = vfs_create_fat32_vnode(path, &vnode);
        if (mount_status != K_OK) return mount_status;
        existed = true;
    }
    if (vnode == 0) {
        vfs_lock();
        vnode = vfs_find_node_locked(path);
        existed = vnode != 0;
        if (vnode != 0) object_get(vnode);
        vfs_unlock();
    }
    if (vnode == 0 && (flags & VFS_OPEN_CREATE) != 0U) {
        kstatus_t create_status = vfs_create_memory_node(
            path, 0U, (flags & VFS_OPEN_DIRECTORY) != 0U);
        if (create_status != K_OK && create_status != K_EEXIST) return create_status;
        vfs_lock();
        vnode = vfs_find_node_locked(path);
        if (vnode != 0) object_get(vnode);
        vfs_unlock();
        if (vnode == 0) {
            kstatus_t mount_status = vfs_create_fat32_vnode(path, &vnode);
            if (mount_status != K_OK) return mount_status;
        }
        existed = create_status == K_EEXIST;
    }
    if (vnode == 0) return K_ENOENT;
    if ((flags & VFS_OPEN_EXCLUSIVE) != 0U &&
        (flags & VFS_OPEN_CREATE) != 0U && existed) {
        object_put(vnode);
        return K_EEXIST;
    }
    vfs_backend_node_t *backend = (vfs_backend_node_t *)vnode->fs_private;
    if (backend == 0 ||
        (backend->directory && (flags & VFS_OPEN_DIRECTORY) == 0U) ||
        (!backend->directory && (flags & VFS_OPEN_DIRECTORY) != 0U) ||
        ((!backend->directory) && (flags & VFS_OPEN_READ) != 0U && backend->read == 0) ||
        ((flags & VFS_OPEN_WRITE) != 0U && backend->write == 0)) {
        object_put(vnode);
        return backend != 0 && backend->directory ? K_EISDIR : K_EACCES;
    }
    if ((flags & VFS_OPEN_TRUNCATE) != 0U) {
        kstatus_t truncate_status = vfs_truncate_node(vnode, 0U);
        if (truncate_status != K_OK) {
            object_put(vnode);
            return truncate_status;
        }
    }
    file_t *file = (file_t *)kzalloc(sizeof(*file), 0);
    if (file == 0) {
        object_put(vnode);
        return K_ENOMEM;
    }
    refcount_init(&file->object.refs, 1U);
    file->object.type = KOBJECT_TYPE_FILE;
    file->object.flags = 0;
    file->object.ops = &g_file_object_ops;
    file->object.security = 0;
    file->vnode = vnode;
    file->ops = 0;
    file->position = (flags & VFS_OPEN_APPEND) != 0U ? vnode->size : 0U;
    file->flags = flags;
    file->rights = flags;
    file->private_data = 0;
    *out = file;
    return K_OK;
}

kstatus_t vfs_open(const char __user *path, uint32_t flags, uint32_t mode,
                   file_t **out) {
    char kernel_path[VFS_PATH_LIMIT];
    if (!vfs_copy_path_from_user(path, kernel_path)) return K_EACCES;
    if ((flags & VFS_OPEN_CREATE) != 0U) {
        kstatus_t status = vfs_create_kernel(kernel_path, mode,
            (flags & VFS_OPEN_DIRECTORY) != 0U);
        if (status != K_OK && status != K_EEXIST) return status;
    }
    return vfs_open_kernel(kernel_path, flags, out);
}

kstatus_t vfs_stat(const char __user *path, os_file_info_t *out) {
    char kernel_path[VFS_PATH_LIMIT];
    if (out == 0 || !vfs_copy_path_from_user(path, kernel_path)) return K_EACCES;
    return vfs_stat_kernel(kernel_path, out);
}

kstatus_t vfs_enumerate(const char __user *path, uint32_t index,
                        os_file_info_t *out) {
    char kernel_path[VFS_PATH_LIMIT];
    if (out == 0 || !vfs_copy_path_from_user(path, kernel_path)) return K_EACCES;
    return vfs_enumerate_kernel(kernel_path, index, out);
}

kstatus_t vfs_remove(const char __user *path) {
    char kernel_path[VFS_PATH_LIMIT];
    if (!vfs_copy_path_from_user(path, kernel_path)) return K_EACCES;
    return vfs_remove_kernel(kernel_path);
}

kstatus_t vfs_mkdir(const char __user *path, uint32_t mode) {
    char kernel_path[VFS_PATH_LIMIT];
    if (!vfs_copy_path_from_user(path, kernel_path)) return K_EACCES;
    return vfs_mkdir_kernel(kernel_path, mode);
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

kstatus_t vfs_read(file_t *file, void __user *buf, size_t len, uint64_t *bytes) {
    if (file == 0 || file->object.type != KOBJECT_TYPE_FILE || file->vnode == 0 ||
        bytes == 0 || (file->rights & VFS_OPEN_READ) == 0U) return K_EINVAL;
    if (len != 0 && !x86_user_range_valid(buf, len)) return K_EINVAL;
    vfs_backend_node_t *backend =
        (vfs_backend_node_t *)file->vnode->fs_private;
    if (backend == 0 || backend->read == 0 || file->position > backend->size) return K_EIO;
    size_t total = 0;
    while (total < len) {
        uint8_t temporary[PAGE_SIZE];
        size_t chunk = len - total;
        if (chunk > sizeof(temporary)) chunk = sizeof(temporary);
        uint64_t got = 0;
        backend_lock(backend);
        kstatus_t status = backend->read(backend->context, file->position,
                                         temporary, chunk, &got);
        backend_unlock(backend);
        if (status != K_OK || got > chunk) return status == K_OK ? K_EIO : status;
        if (got == 0) break;
        if (copy_to_user((uint8_t __user *)buf + total, temporary, (size_t)got) != K_OK) {
            return K_EACCES;
        }
        total += (size_t)got;
        file->position += got;
        if (got < chunk) break;
    }
    *bytes = total;
    return K_OK;
}

kstatus_t vfs_read_kernel(file_t *file, void *buf, size_t len, uint64_t *bytes) {
    if (file == 0 || file->object.type != KOBJECT_TYPE_FILE || file->vnode == 0 ||
        buf == 0 || bytes == 0 || (file->rights & VFS_OPEN_READ) == 0U) return K_EINVAL;
    vfs_backend_node_t *backend =
        (vfs_backend_node_t *)file->vnode->fs_private;
    if (backend == 0 || backend->read == 0 || file->position > backend->size) return K_EIO;
    size_t total = 0;
    while (total < len) {
        size_t chunk = len - total;
        if (chunk > PAGE_SIZE) chunk = PAGE_SIZE;
        uint64_t got = 0;
        backend_lock(backend);
        kstatus_t status = backend->read(backend->context, file->position,
                                         (uint8_t *)buf + total, chunk, &got);
        backend_unlock(backend);
        if (status != K_OK || got > chunk) return status == K_OK ? K_EIO : status;
        if (got == 0) break;
        total += (size_t)got;
        file->position += got;
        if (got < chunk) break;
    }
    *bytes = total;
    return K_OK;
}

kstatus_t vfs_write(file_t *file, const void __user *buf, size_t len, uint64_t *bytes) {
    if (file == 0 || file->object.type != KOBJECT_TYPE_FILE || file->vnode == 0 ||
        bytes == 0 || (file->rights & VFS_OPEN_WRITE) == 0U) return K_EACCES;
    vfs_backend_node_t *backend =
        (vfs_backend_node_t *)file->vnode->fs_private;
    if (backend == 0 || backend->write == 0) return K_EACCES;
    if (len != 0 && !x86_user_range_valid(buf, len)) return K_EINVAL;
    size_t total = 0;
    while (total < len) {
        uint8_t temporary[PAGE_SIZE];
        size_t chunk = len - total;
        if (chunk > sizeof(temporary)) chunk = sizeof(temporary);
        if (copy_from_user(temporary, (const uint8_t __user *)buf + total, chunk) != K_OK) {
            return K_EACCES;
        }
        uint64_t written = 0;
        backend_lock(backend);
        kstatus_t status = backend->write(backend->context, file->position,
                                          temporary, chunk, &written);
        backend_unlock(backend);
        if (status != K_OK || written > chunk) {
            return status == K_OK ? K_EIO : status;
        }
        if (written == 0) break;
        vfs_cache_copy_in(file->vnode, file->position, temporary,
                          (size_t)written);
        total += (size_t)written;
        file->position += written;
        if (written < chunk) break;
    }
    *bytes = total;
    return K_OK;
}

kstatus_t vfs_write_kernel(file_t *file, const void *buf, size_t len,
                           uint64_t *bytes) {
    if (file == 0 || file->object.type != KOBJECT_TYPE_FILE || file->vnode == 0 ||
        (buf == 0 && len != 0) || bytes == 0 ||
        (file->rights & VFS_OPEN_WRITE) == 0U) return K_EACCES;
    vfs_backend_node_t *backend =
        (vfs_backend_node_t *)file->vnode->fs_private;
    if (backend == 0 || backend->write == 0) return K_EACCES;
    size_t total = 0;
    while (total < len) {
        size_t chunk = len - total;
        if (chunk > PAGE_SIZE) chunk = PAGE_SIZE;
        uint64_t written = 0;
        backend_lock(backend);
        kstatus_t status = backend->write(backend->context, file->position,
                                          (const uint8_t *)buf + total, chunk,
                                          &written);
        backend_unlock(backend);
        if (status != K_OK || written > chunk) {
            return status == K_OK ? K_EIO : status;
        }
        if (written == 0) break;
        vfs_cache_copy_in(file->vnode, file->position,
                          (const uint8_t *)buf + total, (size_t)written);
        total += (size_t)written;
        file->position += written;
        if (written < chunk) break;
    }
    *bytes = total;
    return K_OK;
}

kstatus_t vfs_fsync(file_t *file) {
    if (file == 0 || file->object.type != KOBJECT_TYPE_FILE || file->vnode == 0) {
        return K_EINVAL;
    }
    vfs_backend_node_t *backend =
        (vfs_backend_node_t *)file->vnode->fs_private;
    if (backend == 0) return K_EIO;
    vnode_lock(file->vnode);
    kstatus_t status = vnode_page_cache_flush(file->vnode);
    vnode_unlock(file->vnode);
    if (status != K_OK) return status;
    if (backend->fsync == 0) return K_OK;
    backend_lock(backend);
    status = backend->fsync(backend->context);
    backend_unlock(backend);
    return status;
}

void vfs_close(file_t *file) {
    object_put(file);
}
#endif

static BOOLEAN node_access_allowed(const LITEOS_VFS_MANAGER *manager,
                                   const LITEOS_VFS_NODE *node,
                                   UINT32 desired_access) {
    if (manager == 0 || node == 0 || desired_access == 0) return 0;
    if (node->SecurityDescriptor == 0) return 1;
    return manager->HasSecurityToken &&
           liteos_security_access_check(&manager->SecurityToken,
                                        node->SecurityDescriptor,
                                        desired_access);
}

static BOOLEAN copy_string(CHAR8 *destination, UINT32 capacity, const CHAR8 *source) {
    if (destination == 0 || source == 0 || capacity == 0) return 0;
    for (UINT32 i = 0; i + 1U < capacity; ++i) {
        destination[i] = source[i];
        if (source[i] == 0) return 1;
    }
    destination[capacity - 1U] = 0;
    return source[capacity - 1U] == 0;
}

static UINT32 string_length(const CHAR8 *text) {
    UINT32 length = 0;
    while (length < LITEOS_VFS_PATH_LENGTH && text[length] != 0) ++length;
    return length;
}

static BOOLEAN path_prefix_matches(const CHAR8 *path, const CHAR8 *prefix) {
    UINT32 prefix_length = string_length(prefix);
    UINT32 path_length = string_length(path);
    if (prefix_length == 0 || path_length < prefix_length) return 0;
    for (UINT32 i = 0; i < prefix_length; ++i) {
        if (path[i] != prefix[i]) return 0;
    }
    return prefix_length == 1U || path_length == prefix_length ||
           path[prefix_length] == '/';
}

BOOLEAN liteos_vfs_init(LITEOS_VFS_MANAGER *manager) {
    if (manager == 0 || manager->Initialized) return 0;
    for (UINT32 i = 0; i < LITEOS_VFS_MOUNT_COUNT; ++i) {
        manager->Mounts[i].Mounted = 0;
        manager->Mounts[i].Prefix[0] = 0;
        manager->Mounts[i].Lookup = 0;
        manager->Mounts[i].FilesystemContext = 0;
    }
    manager->MountCount = 0;
    manager->SecurityToken.UserId = 0;
    manager->SecurityToken.GroupId = 0;
    manager->SecurityToken.Capabilities = 0;
    manager->HasSecurityToken = 0;
    manager->Initialized = 1;
    return 1;
}

BOOLEAN liteos_vfs_mount(LITEOS_VFS_MANAGER *manager, const CHAR8 *prefix,
                         LITEOS_VFS_LOOKUP lookup, VOID *filesystem_context) {
    if (manager == 0 || !manager->Initialized || prefix == 0 || lookup == 0 ||
        prefix[0] != '/') return 0;
    for (UINT32 i = 0; i < LITEOS_VFS_MOUNT_COUNT; ++i) {
        LITEOS_VFS_MOUNT *mount = &manager->Mounts[i];
        if (mount->Mounted || !copy_string(mount->Prefix, LITEOS_VFS_PATH_LENGTH, prefix)) continue;
        mount->Mounted = 1;
        mount->Lookup = lookup;
        mount->FilesystemContext = filesystem_context;
        ++manager->MountCount;
        return 1;
    }
    return 0;
}

BOOLEAN liteos_vfs_set_security_token(LITEOS_VFS_MANAGER *manager,
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

BOOLEAN liteos_vfs_unmount(LITEOS_VFS_MANAGER *manager, const CHAR8 *prefix) {
    if (manager == 0 || !manager->Initialized || prefix == 0) return 0;
    for (UINT32 i = 0; i < LITEOS_VFS_MOUNT_COUNT; ++i) {
        LITEOS_VFS_MOUNT *mount = &manager->Mounts[i];
        if (!mount->Mounted || !path_prefix_matches(prefix, mount->Prefix) ||
            string_length(prefix) != string_length(mount->Prefix)) continue;
        mount->Mounted = 0;
        mount->Prefix[0] = 0;
        mount->Lookup = 0;
        mount->FilesystemContext = 0;
        if (manager->MountCount != 0) --manager->MountCount;
        return 1;
    }
    return 0;
}

BOOLEAN liteos_vfs_open_access(LITEOS_VFS_MANAGER *manager, const CHAR8 *path,
                               UINT32 desired_access, LITEOS_FILE *file) {
    if (manager == 0 || !manager->Initialized || path == 0 || file == 0 || file->Opened ||
        path[0] != '/' || desired_access == 0U) return 0;
    LITEOS_VFS_MOUNT *best = 0;
    UINT32 best_length = 0;
    for (UINT32 i = 0; i < LITEOS_VFS_MOUNT_COUNT; ++i) {
        LITEOS_VFS_MOUNT *mount = &manager->Mounts[i];
        UINT32 length = string_length(mount->Prefix);
        if (mount->Mounted && length >= best_length && path_prefix_matches(path, mount->Prefix)) {
            best = mount;
            best_length = length;
        }
    }
    if (best == 0) return 0;
    const CHAR8 *relative = path + best_length;
    if (*relative == '/') ++relative;
    if (!best->Lookup(best->FilesystemContext, relative, &file->Node) ||
        !node_access_allowed(manager, &file->Node, desired_access)) {
        if (file->Node.Operations != 0 && file->Node.Operations->Close != 0) {
            file->Node.Operations->Close(&file->Node);
        }
        return 0;
    }
    file->Manager = manager;
    file->Position = 0;
    file->GrantedAccess = desired_access;
    file->Opened = 1;
    return 1;
}

BOOLEAN liteos_vfs_open(LITEOS_VFS_MANAGER *manager, const CHAR8 *path,
                        LITEOS_FILE *file) {
    return liteos_vfs_open_access(manager, path, LITEOS_ACCESS_READ, file);
}

BOOLEAN liteos_vfs_close(LITEOS_FILE *file) {
    if (file == 0 || !file->Opened) return 0;
    if (file->Node.Operations != 0 && file->Node.Operations->Close != 0 &&
        !file->Node.Operations->Close(&file->Node)) return 0;
    file->Opened = 0;
    file->Manager = 0;
    file->GrantedAccess = 0;
    return 1;
}

BOOLEAN liteos_vfs_read(LITEOS_FILE *file, VOID *buffer, UINT32 capacity,
                        UINT32 *read_size) {
    if (file == 0 || !file->Opened || (file->GrantedAccess & LITEOS_ACCESS_READ) == 0U ||
        !node_access_allowed(file->Manager, &file->Node, LITEOS_ACCESS_READ) ||
        file->Node.Operations == 0 || file->Node.Operations->Read == 0) return 0;
    if (!file->Node.Operations->Read(&file->Node, file->Position, buffer, capacity, read_size)) return 0;
    file->Position += *read_size;
    return 1;
}

BOOLEAN liteos_vfs_write(LITEOS_FILE *file, const VOID *buffer, UINT32 size,
                         UINT32 *written_size) {
    if (file == 0 || !file->Opened || (file->GrantedAccess & LITEOS_ACCESS_WRITE) == 0U ||
        !node_access_allowed(file->Manager, &file->Node, LITEOS_ACCESS_WRITE) ||
        file->Node.Operations == 0 || file->Node.Operations->Write == 0) return 0;
    if (!file->Node.Operations->Write(&file->Node, file->Position, buffer, size, written_size)) return 0;
    file->Position += *written_size;
    if (file->Position > file->Node.Size) file->Node.Size = file->Position;
    return 1;
}

BOOLEAN liteos_ramfs_init(LITEOS_RAMFS *ramfs) {
    if (ramfs == 0) return 0;
    for (UINT32 i = 0; i < LITEOS_RAMFS_FILE_COUNT; ++i) {
        ramfs->Files[i].Used = 0;
        ramfs->Files[i].Path[0] = 0;
        ramfs->Files[i].Size = 0;
        ramfs->Files[i].SecurityDescriptor = 0;
    }
    return 1;
}

BOOLEAN liteos_ramfs_create_file(LITEOS_RAMFS *ramfs, const CHAR8 *path,
                                 const VOID *data, UINT32 size) {
    if (ramfs == 0 || path == 0 || path[0] != '/' || size > LITEOS_RAMFS_FILE_SIZE ||
        (size != 0 && data == 0)) return 0;
    for (UINT32 i = 0; i < LITEOS_RAMFS_FILE_COUNT; ++i) {
        LITEOS_RAMFS_FILE *file = &ramfs->Files[i];
        if (file->Used || !copy_string(file->Path, LITEOS_VFS_PATH_LENGTH, path)) continue;
        file->Used = 1;
        file->Size = size;
        file->SecurityDescriptor = 0;
        for (UINT32 byte = 0; byte < size; ++byte) file->Data[byte] = ((const UINT8 *)data)[byte];
        return 1;
    }
    return 0;
}

BOOLEAN liteos_ramfs_set_security_descriptor(
    LITEOS_RAMFS *ramfs, const CHAR8 *path,
    LITEOS_SECURITY_DESCRIPTOR *descriptor) {
    if (ramfs == 0 || path == 0) return 0;
    for (UINT32 i = 0; i < LITEOS_RAMFS_FILE_COUNT; ++i) {
        LITEOS_RAMFS_FILE *file = &ramfs->Files[i];
        if (file->Used && string_length(file->Path) == string_length(path)) {
            UINT32 length = string_length(path);
            UINT32 j = 0;
            while (j < length && file->Path[j] == path[j]) ++j;
            if (j == length) {
                file->SecurityDescriptor = descriptor;
                return 1;
            }
        }
    }
    return 0;
}

static BOOLEAN ramfs_read(LITEOS_VFS_NODE *node, UINT64 offset, VOID *buffer,
                          UINT32 capacity, UINT32 *read_size) {
    LITEOS_RAMFS_FILE *file = (LITEOS_RAMFS_FILE *)node->FileContext;
    if (file == 0 || buffer == 0 || read_size == 0 || offset > file->Size) return 0;
    UINT32 available = file->Size - (UINT32)offset;
    UINT32 count = capacity < available ? capacity : available;
    for (UINT32 i = 0; i < count; ++i) ((UINT8 *)buffer)[i] = file->Data[offset + i];
    *read_size = count;
    return 1;
}

static BOOLEAN ramfs_write(LITEOS_VFS_NODE *node, UINT64 offset, const VOID *buffer,
                           UINT32 size, UINT32 *written_size) {
    LITEOS_RAMFS_FILE *file = (LITEOS_RAMFS_FILE *)node->FileContext;
    if (file == 0 || buffer == 0 || written_size == 0 || offset > LITEOS_RAMFS_FILE_SIZE ||
        size > LITEOS_RAMFS_FILE_SIZE - (UINT32)offset) return 0;
    for (UINT32 i = 0; i < size; ++i) file->Data[offset + i] = ((const UINT8 *)buffer)[i];
    if (offset + size > file->Size) file->Size = (UINT32)(offset + size);
    node->Size = file->Size;
    *written_size = size;
    return 1;
}

static const LITEOS_VFS_FILE_OPERATIONS g_ramfs_operations = {
    ramfs_read,
    ramfs_write,
    0,
};

BOOLEAN liteos_ramfs_lookup(VOID *filesystem_context, const CHAR8 *path,
                            LITEOS_VFS_NODE *node) {
    LITEOS_RAMFS *ramfs = (LITEOS_RAMFS *)filesystem_context;
    if (ramfs == 0 || path == 0 || node == 0) return 0;
    CHAR8 full_path[LITEOS_VFS_PATH_LENGTH];
    UINT32 path_length = string_length(path);
    if (path_length + 1U >= LITEOS_VFS_PATH_LENGTH) return 0;
    full_path[0] = '/';
    for (UINT32 i = 0; i <= path_length; ++i) full_path[i + 1U] = path[i];
    for (UINT32 i = 0; i < LITEOS_RAMFS_FILE_COUNT; ++i) {
        LITEOS_RAMFS_FILE *file = &ramfs->Files[i];
        if (!file->Used) continue;
        UINT32 j = 0;
        while (full_path[j] != 0 && file->Path[j] == full_path[j]) ++j;
        if (full_path[j] == 0 && file->Path[j] == 0) {
            node->Type = 1U;
            node->Size = file->Size;
            node->FilesystemContext = ramfs;
            node->FileContext = file;
            node->SecurityDescriptor = file->SecurityDescriptor;
            node->Operations = &g_ramfs_operations;
            return 1;
        }
    }
    return 0;
}
