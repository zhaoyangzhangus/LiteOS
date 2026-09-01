#include <kernel/kmem.h>
#include <kernel/object.h>
#include <kernel/vfs.h>
#include <kernel/fat32.h>
#include "internal.h"

#ifdef LITEOS_KERNEL_BUILD
#define VFS_MEMORY_NODE_LIMIT 64U

static vnode_t *g_vfs_nodes[VFS_MEMORY_NODE_LIMIT];
static spinlock_t g_vfs_lock;
static atomic_uint g_vfs_init_state;
vfs_fat32_mount_t g_fat32_mounts[VFS_FAT32_MOUNT_LIMIT];

static void vfs_lock(void) {
    while (atomic_exchange_explicit(&g_vfs_lock.state, 1U,
                                    memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
}

static void vfs_unlock(void) {
    atomic_store_explicit(&g_vfs_lock.state, 0U, memory_order_release);
}

size_t vfs_string_length(const char *text) {
    size_t length = 0;
    if (text == 0) return 0;
    while (length < VFS_PATH_LIMIT && text[length] != 0) ++length;
    return length;
}

static void vnode_destroy(void *object) {
    vnode_t *vnode = (vnode_t *)object;
    vfs_backend_node_t *backend = vnode != 0 ?
                                  (vfs_backend_node_t *)vnode->fs_private : 0;
    if (vnode != 0) {
        /* 最后一个 vnode 引用释放时仍要尽力写回 mmap 脏页。 */
        vfs_vnode_lock(vnode);
        (void)vfs_page_cache_flush(vnode);
        vfs_vnode_unlock(vnode);
        vfs_page_cache_destroy(vnode);
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

static bool vfs_fat_path_equal(const char *left, const char *right) {
    char left_relative[VFS_PATH_LIMIT];
    char right_relative[VFS_PATH_LIMIT];
    size_t length;
    if (!vfs_fat32_relative_path(left, left_relative) ||
        !vfs_fat32_relative_path(right, right_relative)) return false;
    length = vfs_string_length(left_relative);
    if (length != vfs_string_length(right_relative)) return false;
    for (size_t index = 0U; index < length; ++index) {
        char left_value = left_relative[index];
        char right_value = right_relative[index];
        if (left_value >= 'A' && left_value <= 'Z') {
            left_value = (char)(left_value - 'A' + 'a');
        }
        if (right_value >= 'A' && right_value <= 'Z') {
            right_value = (char)(right_value - 'A' + 'a');
        }
        if (left_value != right_value) return false;
    }
    return true;
}

static vnode_t *vfs_find_fat_node_locked(const char *path) {
    for (uint32_t index = 0U; index < VFS_MEMORY_NODE_LIMIT; ++index) {
        vnode_t *vnode = g_vfs_nodes[index];
        vfs_backend_node_t *backend = vnode != 0 ?
                                      (vfs_backend_node_t *)vnode->fs_private : 0;
        if (backend != 0 && backend->fat32_node &&
            vfs_fat_path_equal(path, backend->path)) return vnode;
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

static bool vfs_mount_matches(const vfs_fat32_mount_t *mount,
                              const char *path, size_t path_length) {
    size_t prefix_length;

    if (mount == 0 || !mount->mounted || mount->filesystem == 0 ||
        path == 0 || path_length == 0U || path[0] != '/') return false;
    prefix_length = vfs_string_length(mount->prefix);
    if (prefix_length == 0U || path_length < prefix_length ||
        !vfs_path_range_equals(path, prefix_length,
                               mount->prefix, prefix_length)) return false;
    return prefix_length == 1U || path_length == prefix_length ||
           path[prefix_length] == '/';
}

vfs_fat32_mount_t *vfs_fat32_mount_for_path(const char *path,
                                             char relative[VFS_PATH_LIMIT]) {
    vfs_fat32_mount_t *selected = 0;
    size_t path_length;
    size_t selected_length = 0U;

    if (path == 0 || relative == 0 || path[0] != '/') return 0;
    path_length = vfs_string_length(path);
    if (path_length == 0U || path_length >= VFS_PATH_LIMIT) return 0;
    for (uint32_t index = 0U; index < VFS_FAT32_MOUNT_LIMIT; ++index) {
        vfs_fat32_mount_t *candidate = &g_fat32_mounts[index];
        size_t prefix_length = vfs_string_length(candidate->prefix);
        if (!vfs_mount_matches(candidate, path, path_length) ||
            prefix_length <= selected_length) continue;
        selected = candidate;
        selected_length = prefix_length;
    }
    if (selected == 0) return 0;

    if (path_length == selected_length) {
        relative[0] = 0;
    } else if (selected_length == 1U) {
        size_t length = path_length - 1U;
        if (length >= VFS_PATH_LIMIT) return 0;
        for (size_t index = 0U; index < length; ++index) {
            relative[index] = path[index + 1U];
        }
        relative[length] = 0;
    } else {
        size_t length = path_length - selected_length - 1U;
        if (length >= VFS_PATH_LIMIT) return 0;
        for (size_t index = 0U; index < length; ++index) {
            relative[index] = path[selected_length + 1U + index];
        }
        relative[length] = 0;
    }
    return selected;
}

bool vfs_fat32_relative_path(const char *path, char relative[VFS_PATH_LIMIT]) {
    return vfs_fat32_mount_for_path(path, relative) != 0;
}

static bool vfs_fat32_stat_absolute(const char *path, os_file_info_t *info,
                                    char relative[VFS_PATH_LIMIT]) {
    vfs_fat32_mount_t *mount = vfs_fat32_mount_for_path(path, relative);
    return mount != 0 && liteos_fat32_stat_path(mount->filesystem, relative, info);
}

kstatus_t vfs_mount_fat32(const char *prefix, LITEOS_FAT32 *filesystem) {
    size_t length;
    if (prefix == 0 || filesystem == 0 || !filesystem->Mounted || prefix[0] != '/') {
        return K_EINVAL;
    }
    length = vfs_string_length(prefix);
    if (length == 0U || length >= VFS_PATH_LIMIT ||
        (length > 1U && prefix[length - 1U] == '/')) {
        return K_EINVAL;
    }
    vfs_initialize();
    vfs_lock();
    bool root_mount = length == 1U;
    uint32_t slot = root_mount ? 0U : VFS_FAT32_MOUNT_LIMIT;
    uint32_t first_slot = root_mount ? 0U : 1U;
    for (uint32_t index = first_slot; index < VFS_FAT32_MOUNT_LIMIT; ++index) {
        if (g_fat32_mounts[index].mounted &&
            vfs_path_range_equals(g_fat32_mounts[index].prefix,
                                  vfs_string_length(g_fat32_mounts[index].prefix),
                                  prefix, length)) {
            vfs_unlock();
            return K_EBUSY;
        }
        if (slot == VFS_FAT32_MOUNT_LIMIT &&
            !g_fat32_mounts[index].mounted) slot = index;
    }
    if (slot == VFS_FAT32_MOUNT_LIMIT) {
        vfs_unlock();
        return K_ENOMEM;
    }
    for (size_t index = 0U; index <= length; ++index) {
        g_fat32_mounts[slot].prefix[index] = prefix[index];
    }
    g_fat32_mounts[slot].filesystem = filesystem;
    g_fat32_mounts[slot].mounted = true;
    vfs_unlock();
    return K_OK;
}

kstatus_t vfs_unmount_fat32(void) {
    vfs_initialize();
    vfs_lock();
    if (!g_fat32_mounts[0].mounted) {
        vfs_unlock();
        return K_ENOENT;
    }
    g_fat32_mounts[0].prefix[0] = 0;
    g_fat32_mounts[0].filesystem = 0;
    g_fat32_mounts[0].mounted = false;
    vfs_unlock();
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

kstatus_t vfs_file_stat(file_t *file, os_file_info_t *out) {
    vnode_t *vnode;
    vfs_backend_node_t *backend;
    if (file == 0 || out == 0 || file->object.type != KOBJECT_TYPE_FILE ||
        file->vnode == 0) return K_EBADF;
    vnode = file->vnode;
    backend = (vfs_backend_node_t *)vnode->fs_private;
    if (backend == 0) return K_EIO;
    vfs_initialize();
    vfs_lock();
    vfs_fill_info(backend->path, vnode, backend, out);
    vfs_unlock();
    return K_OK;
}

static bool vfs_path_is_subtree(const char *path, size_t path_length,
                                const char *root, size_t root_length) {
    return path_length == root_length ||
           (path_length > root_length && path[root_length] == '/' &&
            vfs_path_range_equals(path, root_length, root, root_length));
}

static void vfs_rekey_cached_subtree_locked(const char *old_path,
                                            size_t old_length,
                                            const char *new_path,
                                            size_t new_length) {
    for (uint32_t index = 0U; index < VFS_MEMORY_NODE_LIMIT; ++index) {
        vnode_t *candidate = g_vfs_nodes[index];
        vfs_backend_node_t *backend = candidate != 0 ?
            (vfs_backend_node_t *)candidate->fs_private : 0;
        size_t candidate_length;
        size_t suffix_length;
        char replacement[VFS_PATH_LIMIT];
        if (backend == 0) continue;
        candidate_length = vfs_string_length(backend->path);
        if (!vfs_path_is_subtree(backend->path, candidate_length,
                                 old_path, old_length)) continue;
        suffix_length = candidate_length - old_length;
        if (new_length + suffix_length >= VFS_PATH_LIMIT) continue;
        __builtin_memcpy(replacement, new_path, new_length);
        __builtin_memcpy(replacement + new_length, backend->path + old_length,
                         suffix_length);
        replacement[new_length + suffix_length] = '\0';
        __builtin_memcpy(backend->path, replacement,
                         new_length + suffix_length + 1U);
    }
}

kstatus_t vfs_rename_kernel(const char *old_path, const char *new_path) {
    char old_relative[VFS_PATH_LIMIT];
    char new_relative[VFS_PATH_LIMIT];
    size_t old_length;
    size_t new_length;
    bool old_mounted;
    bool new_mounted;
    vfs_fat32_mount_t *old_mount;
    vfs_fat32_mount_t *new_mount;

    if (old_path == 0 || new_path == 0 || old_path[0] != '/' ||
        new_path[0] != '/') return K_EINVAL;
    old_length = vfs_string_length(old_path);
    new_length = vfs_string_length(new_path);
    if (old_length <= 1U || new_length <= 1U ||
        old_length >= VFS_PATH_LIMIT || new_length >= VFS_PATH_LIMIT ||
        old_path[old_length - 1U] == '/' || new_path[new_length - 1U] == '/') {
        return K_EINVAL;
    }
    if (vfs_path_range_equals(old_path, old_length, new_path, new_length)) {
        return K_OK;
    }

    old_mount = vfs_fat32_mount_for_path(old_path, old_relative);
    new_mount = vfs_fat32_mount_for_path(new_path, new_relative);
    old_mounted = old_mount != 0;
    new_mounted = new_mount != 0;
    if (old_mounted || new_mounted) {
        if (!old_mounted || !new_mounted || old_mount != new_mount ||
            old_relative[0] == 0 ||
            new_relative[0] == 0) return K_EXDEV;
        if (!liteos_fat32_rename_path(old_mount->filesystem,
                                      old_relative, new_relative)) {
            return K_EIO;
        }
        /* FAT keeps the inode alive across a rename.  Rekey cached vnodes so
         * already-open files continue to use the new path for later I/O and
         * fstat, while fresh opens do not retain the old cache key. */
        vfs_initialize();
        vfs_lock();
        vfs_rekey_cached_subtree_locked(old_path, old_length,
                                        new_path, new_length);
        vfs_unlock();
        return K_OK;
    }

    vfs_initialize();
    vfs_lock();
    vnode_t *source = vfs_find_node_locked(old_path);
    vfs_backend_node_t *source_backend = source != 0 ?
        (vfs_backend_node_t *)source->fs_private : 0;
    if (source_backend == 0) {
        vfs_unlock();
        return K_ENOENT;
    }
    if (!source_backend->mutable_node) {
        vfs_unlock();
        return K_EACCES;
    }
    if (!vfs_parent_directory_exists_locked(new_path, new_length)) {
        vfs_unlock();
        return K_ENOTDIR;
    }
    if (vfs_find_node_locked(new_path) != 0 ||
        vfs_directory_exists_locked(new_path, new_length)) {
        vfs_unlock();
        return K_EEXIST;
    }
    if (source_backend->directory &&
        vfs_path_is_subtree(new_path, new_length, old_path, old_length)) {
        vfs_unlock();
        return K_EINVAL;
    }

    /* Validate every descendant before changing any path, so a failed rename
     * cannot leave a partially moved in-memory directory tree. */
    for (uint32_t index = 0U; index < VFS_MEMORY_NODE_LIMIT; ++index) {
        vnode_t *candidate = g_vfs_nodes[index];
        vfs_backend_node_t *backend = candidate != 0 ?
            (vfs_backend_node_t *)candidate->fs_private : 0;
        size_t candidate_length;
        size_t suffix_length;
        if (backend == 0) continue;
        candidate_length = vfs_string_length(backend->path);
        if (!vfs_path_is_subtree(backend->path, candidate_length,
                                 old_path, old_length)) continue;
        suffix_length = candidate_length - old_length;
        if (new_length > VFS_PATH_LIMIT - 1U - suffix_length) {
            vfs_unlock();
            return K_ENAMETOOLONG;
        }
        if (!backend->mutable_node) {
            vfs_unlock();
            return K_EACCES;
        }
    }
    for (uint32_t index = 0U; index < VFS_MEMORY_NODE_LIMIT; ++index) {
        vnode_t *candidate = g_vfs_nodes[index];
        vfs_backend_node_t *backend = candidate != 0 ?
            (vfs_backend_node_t *)candidate->fs_private : 0;
        size_t candidate_length;
        size_t suffix_length;
        char replacement[VFS_PATH_LIMIT];
        if (backend == 0) continue;
        candidate_length = vfs_string_length(backend->path);
        if (!vfs_path_is_subtree(backend->path, candidate_length,
                                 old_path, old_length)) continue;
        suffix_length = candidate_length - old_length;
        __builtin_memcpy(replacement, new_path, new_length);
        __builtin_memcpy(replacement + new_length, backend->path + old_length,
                         suffix_length);
        replacement[new_length + suffix_length] = '\0';
        __builtin_memcpy(backend->path, replacement,
                         new_length + suffix_length + 1U);
    }
    vfs_unlock();
    return K_OK;
}

static bool vfs_append_entry(os_file_info_t entries[VFS_MEMORY_NODE_LIMIT],
                             uint32_t *entry_count,
                             const os_file_info_t *entry) {
    if (entries == 0 || entry_count == 0 || entry == 0 || entry->name[0] == 0) {
        return false;
    }
    for (uint32_t index = 0U; index < *entry_count; ++index) {
        if (!vfs_path_range_equals(entries[index].name,
                                   vfs_string_length(entries[index].name),
                                   entry->name, vfs_string_length(entry->name))) {
            continue;
        }
        if (entry->type == OS_FILE_TYPE_DIRECTORY) entries[index] = *entry;
        return true;
    }
    if (*entry_count >= VFS_MEMORY_NODE_LIMIT) return false;
    entries[*entry_count] = *entry;
    ++*entry_count;
    return true;
}

static bool vfs_mount_parent_exists(const char *path, size_t path_length) {
    for (uint32_t index = 0U; index < VFS_FAT32_MOUNT_LIMIT; ++index) {
        vfs_fat32_mount_t *mount = &g_fat32_mounts[index];
        size_t prefix_length;
        size_t leaf_start;
        size_t parent_length;
        if (!mount->mounted) continue;
        prefix_length = vfs_string_length(mount->prefix);
        if (prefix_length <= 1U) continue;
        leaf_start = prefix_length;
        while (leaf_start != 0U && mount->prefix[leaf_start - 1U] != '/') {
            --leaf_start;
        }
        parent_length = leaf_start <= 1U ? 1U : leaf_start - 1U;
        if (parent_length == path_length &&
            vfs_path_range_equals(path, path_length,
                                  mount->prefix, parent_length)) return true;
    }
    return false;
}

static bool vfs_append_mount_children(const char *path, size_t path_length,
                                      os_file_info_t entries[VFS_MEMORY_NODE_LIMIT],
                                      uint32_t *entry_count,
                                      bool *directory_exists) {
    for (uint32_t index = 0U; index < VFS_FAT32_MOUNT_LIMIT; ++index) {
        vfs_fat32_mount_t *mount = &g_fat32_mounts[index];
        size_t prefix_length;
        size_t leaf_start;
        size_t parent_length;
        os_file_info_t entry = {0};

        if (!mount->mounted) continue;
        prefix_length = vfs_string_length(mount->prefix);
        if (prefix_length <= 1U) continue;
        leaf_start = prefix_length;
        while (leaf_start != 0U && mount->prefix[leaf_start - 1U] != '/') {
            --leaf_start;
        }
        parent_length = leaf_start <= 1U ? 1U : leaf_start - 1U;
        if (parent_length == 1U && mount->prefix[0] == '/') {
            if (path_length != 1U || path[0] != '/') continue;
        } else if (path_length != parent_length ||
                   !vfs_path_range_equals(path, path_length,
                                          mount->prefix, parent_length)) {
            continue;
        }
        if (directory_exists != 0) *directory_exists = true;
        if (prefix_length - leaf_start >= OS_FILE_NAME_MAX) return false;
        entry.type = OS_FILE_TYPE_DIRECTORY;
        entry.mode = 0040755U;
        for (size_t character = leaf_start; character < prefix_length; ++character) {
            entry.name[character - leaf_start] = mount->prefix[character];
        }
        entry.name[prefix_length - leaf_start] = 0;
        if (!vfs_append_entry(entries, entry_count, &entry)) return false;
    }
    return true;
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
    char relative[VFS_PATH_LIMIT];
    vfs_fat32_mount_t *mount;
    os_file_info_t mounted_info;

    if (path == 0 || out == 0 || path[0] != '/') return K_EINVAL;
    directory_length = vfs_string_length(path);
    if (directory_length == 0U || directory_length >= VFS_PATH_LIMIT) return K_EINVAL;
    while (directory_length > 1U && path[directory_length - 1U] == '/') --directory_length;
    for (uint32_t entry_index = 0U; entry_index < VFS_MEMORY_NODE_LIMIT; ++entry_index) {
        entries[entry_index] = (os_file_info_t){0};
    }

    mount = vfs_fat32_mount_for_path(path, relative);
    bool mounted_directory = false;
    if (mount != 0) {
        if (relative[0] == 0) {
            mounted_info = (os_file_info_t){
                .type = OS_FILE_TYPE_DIRECTORY, .mode = 0040755U
            };
            mounted_directory = true;
        } else {
            mounted_directory =
                liteos_fat32_stat_path(mount->filesystem, relative,
                                        &mounted_info);
        }
    }
    if (mounted_directory) {
        if (mounted_info.type != OS_FILE_TYPE_DIRECTORY) return K_EINVAL;
        directory_exists = true;
        for (uint32_t fat_index = 0U;
             fat_index < VFS_MEMORY_NODE_LIMIT; ++fat_index) {
            os_file_info_t entry = {0};
            if (!liteos_fat32_enumerate_path(mount->filesystem, relative,
                                             fat_index, &entry)) break;
            if (!vfs_append_entry(entries, &entry_count, &entry)) return K_ENOMEM;
        }
    }
    if (!vfs_append_mount_children(path, directory_length, entries, &entry_count,
                                   &directory_exists)) {
        return K_ENOMEM;
    }
    if (directory_length == 1U) directory_exists = true;

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
        os_file_info_t entry = {0};
        for (size_t character = 0U; character < name_length; ++character) {
            entry.name[character] = remainder[character];
        }
        entry.name[name_length] = 0;
        bool child_directory = backend->directory || remainder[name_length] == '/';
        entry.type = child_directory ? OS_FILE_TYPE_DIRECTORY : OS_FILE_TYPE_REGULAR;
        entry.mode = child_directory ?
            (backend->directory ? vnode->mode : 0040555U) : vnode->mode;
        entry.size = child_directory ? 0U : vnode->size;
        if (!vfs_append_entry(entries, &entry_count, &entry)) {
            vfs_unlock();
            return K_ENOMEM;
        }
    }
    vfs_unlock();
    if (!directory_exists) return exact_regular_file ? K_EINVAL : K_ENOENT;
    if (index >= entry_count) return K_ENOENT;
    *out = entries[index];
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
    vfs_fat32_mount_t *mount;
    vfs_backend_node_t *backend;
    vnode_t *vnode;
    if (out == 0 || (mount = vfs_fat32_mount_for_path(path, relative)) == 0) {
        return K_ENOENT;
    }
    if (relative[0] == 0) {
        info = (os_file_info_t){
            .type = OS_FILE_TYPE_DIRECTORY, .mode = 0040755U
        };
    } else if (!liteos_fat32_stat_path(mount->filesystem, relative, &info) ||
               info.type == OS_FILE_TYPE_UNKNOWN) {
        if (!vfs_mount_parent_exists(path, vfs_string_length(path))) {
            return K_ENOENT;
        }
        info = (os_file_info_t){
            .type = OS_FILE_TYPE_DIRECTORY, .mode = 0040755U
        };
    }
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
    backend->fat32_node = true;
    backend->read = backend->directory ? 0 : vfs_fat32_backend_read;
    backend->write = backend->directory ? 0 : vfs_fat32_backend_write;
    backend->fsync = backend->directory ? 0 : vfs_fat32_backend_fsync;
    backend->truncate = backend->directory ? 0 : vfs_fat32_backend_truncate;
    backend->context = backend;
    atomic_init(&backend->io_lock.state, 0U);
    vnode = vfs_create_vnode_object(backend);
    if (vnode == 0) {
        kfree(backend);
        return K_ENOMEM;
    }
    /*
     * Prefer a canonical FAT vnode so mmap and direct I/O share one page
     * cache.  The registry is only a cache, though: exhausting its fixed
     * slots must never make an otherwise valid disk file impossible to open.
     */
    vfs_initialize();
    vfs_lock();
    vnode_t *existing = vfs_find_fat_node_locked(path);
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
        /*
         * All canonical slots are occupied.  Return this vnode uncached
         * instead of turning the 65th distinct FAT path into K_ENOMEM.
         * Existing cached paths remain canonical; this transient vnode is
         * destroyed when its last file/mapping reference is released.
         */
        vfs_unlock();
        *out = vnode;
        return K_OK;
    }
    g_vfs_nodes[slot] = vnode;
    object_get(vnode);
    vfs_unlock();
    *out = vnode;
    return K_OK;
}

kstatus_t vfs_register_node(vfs_backend_node_t *backend) {
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

static kstatus_t vfs_create_memory_node(const char *path, uint32_t mode,
                                        bool directory) {
    vfs_backend_node_t *backend;
    size_t path_length;
    char relative[VFS_PATH_LIMIT];
    os_file_info_t existing_info;
    vfs_fat32_mount_t *mount;
    bool exists;
    bool parent;
    if (path == 0 || path[0] != '/') return K_EINVAL;
    path_length = vfs_string_length(path);
    if (path_length <= 1U || path_length >= VFS_PATH_LIMIT ||
        path[path_length - 1U] == '/') return K_EINVAL;
    mount = vfs_fat32_mount_for_path(path, relative);
    if (mount != 0) {
        if (relative[0] == 0) return K_EBUSY;
        if (liteos_fat32_stat_path(mount->filesystem, relative, &existing_info)) {
            return K_EEXIST;
        }
        if (!liteos_fat32_create_path(mount->filesystem, relative, directory)) {
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
    backend->read = directory ? 0 : vfs_memory_backend_read;
    backend->write = directory ? 0 : vfs_memory_backend_write;
    backend->fsync = 0;
    backend->truncate = directory ? 0 : vfs_memory_backend_truncate;
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

kstatus_t vfs_stat_kernel(const char *path, os_file_info_t *out) {
    size_t path_length;
    char relative[VFS_PATH_LIMIT];
    vfs_fat32_mount_t *mount;
    if (path == 0 || out == 0 || path[0] != '/') return K_EINVAL;
    path_length = vfs_string_length(path);
    if (path_length == 0U || path_length >= VFS_PATH_LIMIT) return K_EINVAL;
    while (path_length > 1U && path[path_length - 1U] == '/') --path_length;
    mount = vfs_fat32_mount_for_path(path, relative);
    if (mount != 0 && relative[0] == 0) {
        *out = (os_file_info_t){
            .type = OS_FILE_TYPE_DIRECTORY, .mode = 0040755U
        };
        vfs_set_leaf_name(path, out->name);
        return K_OK;
    }
    if (mount != 0) {
        bool found = liteos_fat32_stat_path(mount->filesystem, relative, out);
        if (found) return K_OK;
        if (!vfs_mount_parent_exists(path, path_length)) return K_ENOENT;
        *out = (os_file_info_t){
            .type = OS_FILE_TYPE_DIRECTORY, .mode = 0040755U
        };
        vfs_set_leaf_name(path, out->name);
        return K_OK;
    }
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
    vfs_fat32_mount_t *mount;
    os_file_info_t mounted_info;
    if (path == 0 || path[0] != '/') return K_EINVAL;
    path_length = vfs_string_length(path);
    if (path_length <= 1U || path_length >= VFS_PATH_LIMIT) return K_EINVAL;
    while (path_length > 1U && path[path_length - 1U] == '/') --path_length;
    if (path_length <= 1U) return K_EINVAL;
    mount = vfs_fat32_mount_for_path(path, relative);
    if (mount != 0) {
        vnode_t *mounted_victim = 0;
        vnode_t *detached_victims[VFS_MEMORY_NODE_LIMIT] = {0};
        uint32_t detached_count = 0U;
        vfs_backend_node_t *mounted_backend = 0;
        if (relative[0] == 0 ||
            !liteos_fat32_stat_path(mount->filesystem, relative,
                                    &mounted_info)) {
            return K_ENOENT;
        }
        vfs_initialize();
        vfs_lock();
        mounted_victim = vfs_find_fat_node_locked(path);
        if (mounted_victim != 0) {
            object_get(mounted_victim);
            mounted_backend = (vfs_backend_node_t *)mounted_victim->fs_private;
        }
        vfs_unlock();
        if (mounted_backend != 0 &&
            vfs_fat32_backend_prepare_unlink(mounted_backend) != K_OK) {
            object_put(mounted_victim);
            return K_EIO;
        }
        if (!liteos_fat32_remove_path(mount->filesystem, relative)) {
            if (mounted_backend != 0) {
                vfs_fat32_backend_abort_unlink(mounted_backend);
            }
            if (mounted_victim != 0) object_put(mounted_victim);
            return mounted_info.type == OS_FILE_TYPE_DIRECTORY ? K_ENOTEMPTY : K_EIO;
        }
        if (mounted_backend != 0) {
            vfs_fat32_backend_commit_unlink(mounted_backend);
        }
        /* 删除磁盘目录项后同步摘除规范 vnode；仍被打开的 file 引用会让
         * vnode 保持有效，但新的 open 不会再看到已经删除的路径。 */
        vfs_lock();
        for (uint32_t node_index = 0U; node_index < VFS_MEMORY_NODE_LIMIT; ++node_index) {
            vnode_t *candidate = g_vfs_nodes[node_index];
            vfs_backend_node_t *backend = candidate != 0 ?
                                          (vfs_backend_node_t *)candidate->fs_private : 0;
            size_t node_length;
            if (backend == 0) continue;
            node_length = vfs_string_length(backend->path);
            if (!backend->fat32_node ||
                !vfs_fat_path_equal(backend->path, path) ||
                node_length == 0U) {
                continue;
            }
            g_vfs_nodes[node_index] = 0;
            if (detached_count < VFS_MEMORY_NODE_LIMIT) {
                detached_victims[detached_count++] = candidate;
            }
        }
        vfs_unlock();
        for (uint32_t index = 0U; index < detached_count; ++index) {
            object_put(detached_victims[index]);
        }
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

kstatus_t vfs_open_kernel(const char *path, uint32_t flags, uint32_t mode,
                          file_t **out) {
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
    vfs_fat32_mount_t *mount = vfs_fat32_mount_for_path(path, relative);
    bool mounted_path = mount != 0;
    bool mounted_exists = mounted_path &&
        (relative[0] == 0 ||
         liteos_fat32_stat_path(mount->filesystem, relative, &mounted_info));
    bool mounted_parent = mounted_path && !mounted_exists &&
                          vfs_mount_parent_exists(path, vfs_string_length(path));
    if (mounted_path && relative[0] == 0) {
        mounted_info = (os_file_info_t){
            .type = OS_FILE_TYPE_DIRECTORY, .mode = 0040755U
        };
    }
    if (mounted_exists || mounted_parent) {
        kstatus_t mount_status = vfs_create_fat32_vnode(path, &vnode);
        if (mount_status != K_OK) return mount_status;
        existed = mounted_exists;
    }
    /* A root FAT mount covers disk paths, but exact in-memory backends under
     * /native remain valid namespace entries.  Look them up when the
     * mounted filesystem did not provide the requested path. */
    if (vnode == 0) {
        vfs_lock();
        vnode = vfs_find_node_locked(path);
        if (vnode != 0) {
            vfs_backend_node_t *backend =
                (vfs_backend_node_t *)vnode->fs_private;
            if (mounted_path && !mounted_exists && backend != 0 &&
                backend->fat32_node) {
                vnode = 0;
            } else {
                existed = true;
                object_get(vnode);
            }
        }
        vfs_unlock();
    }
    if (vnode == 0 && (flags & VFS_OPEN_CREATE) != 0U) {
        kstatus_t create_status = vfs_create_memory_node(
            path, mode, (flags & VFS_OPEN_DIRECTORY) != 0U);
        if (create_status != K_OK && create_status != K_EEXIST) return create_status;
        if (!mounted_path) {
            vfs_lock();
            vnode = vfs_find_node_locked(path);
            if (vnode != 0) object_get(vnode);
            vfs_unlock();
        }
        if (vnode == 0 && mounted_path) {
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
    file->vnode = vnode;
    file->ops = 0;
    file->position = (flags & VFS_OPEN_APPEND) != 0U ? vnode->size : 0U;
    file->flags = flags;
    file->rights = flags;
    file->private_data = 0;
    *out = file;
    return K_OK;
}

#endif
