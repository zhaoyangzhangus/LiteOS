#include <kernel/kmem.h>
#include <kernel/object.h>
#include <kernel/vfs.h>
#include <kernel/fat32.h>
#include <kernel/string.h>
#include "internal.h"

#ifdef OS_KERNEL_BUILD
#define VFS_DIRECTORY_ENTRY_LIMIT 256U
#define VFS_INITIAL_NODE_CAPACITY 256U

static vnode_t *g_vfs_initial_nodes[VFS_INITIAL_NODE_CAPACITY];
static vnode_t **g_vfs_nodes = g_vfs_initial_nodes;
static uint32_t g_vfs_node_capacity = VFS_INITIAL_NODE_CAPACITY;
static uint32_t g_vfs_next_free_node;
static spinlock_t g_vfs_lock;
static atomic_uint g_vfs_init_state;
/* Linux fstat needs distinct file identities while loading multiple DSOs. */
static atomic_uint_fast64_t g_vfs_next_inode_id;
vfs_fat32_mount_t g_fat32_mounts[VFS_FAT32_MOUNT_LIMIT];

static void vfs_lock(void) {
    vfs_state_lock(&g_vfs_lock.state);
}

static void vfs_unlock(void) {
    vfs_state_unlock(&g_vfs_lock.state);
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
        if (backend->fat_file != 0) {
            (void)fat32_close(backend->fat_file);
            backend->fat_file = 0;
        }
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
    if (atomic_load_explicit(&g_vfs_init_state, memory_order_acquire) == 2U) return;
    unsigned expected = 0U;
    if (atomic_compare_exchange_strong_explicit(&g_vfs_init_state, &expected, 1U,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        atomic_init(&g_vfs_lock.state, 0U);
        for (uint32_t i = 0; i < g_vfs_node_capacity; ++i) g_vfs_nodes[i] = 0;
        g_vfs_next_free_node = 0U;
        atomic_store_explicit(&g_vfs_init_state, 2U, memory_order_release);
        return;
    }
    while (atomic_load_explicit(&g_vfs_init_state, memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
}

static vnode_t *vfs_find_node_locked(const char *path) {
    size_t path_length = vfs_string_length(path);
    for (uint32_t i = 0; i < g_vfs_node_capacity; ++i) {
        vnode_t *vnode = g_vfs_nodes[i];
        vfs_backend_node_t *backend = vnode != 0 ?
                                      (vfs_backend_node_t *)vnode->fs_private : 0;
        if (backend == 0) continue;
        size_t j = 0;
        while (j < path_length && path[j] == backend->path[j]) ++j;
        /* Reject a prefix match without scanning the candidate twice. At the
         * bound, preserve the original nonterminated-path comparison. */
        if (j == path_length && (path_length == VFS_PATH_LIMIT ||
                                 backend->path[path_length] == 0)) return vnode;
    }
    return 0;
}

/* The caller can reuse its normalized request while scanning the registry. */
static bool vfs_fat_relative_path_equal(const char *left_relative,
                                         size_t length,
                                         const vfs_fat32_mount_t *left_mount,
                                         const char *right) {
    char right_relative[VFS_PATH_LIMIT];
    vfs_fat32_mount_t *right_mount = vfs_fat32_mount_for_path(right, right_relative);
    /* Relative names are identities only within the same filesystem. Alias
     * mounts of that filesystem may still share a canonical page cache. */
    if (right_mount == 0 || right_mount->filesystem != left_mount->filesystem)
        return false;
    if (length >= VFS_PATH_LIMIT) return false;
    for (size_t index = 0U; index < length; ++index) {
        char right_value = right_relative[index];
        if (right_value == 0) return false;
        char left_value = left_relative[index];
        if (left_value >= 'A' && left_value <= 'Z') {
            left_value = (char)(left_value - 'A' + 'a');
        }
        if (right_value >= 'A' && right_value <= 'Z') {
            right_value = (char)(right_value - 'A' + 'a');
        }
        if (left_value != right_value) return false;
    }
    return right_relative[length] == 0;
}

static vnode_t *vfs_find_fat_node_locked(const char *path) {
    char relative[VFS_PATH_LIMIT];
    vfs_fat32_mount_t *mount = vfs_fat32_mount_for_path(path, relative);
    if (mount == 0) return 0;
    size_t relative_length = vfs_string_length(relative);
    for (uint32_t index = 0U; index < g_vfs_node_capacity; ++index) {
        vnode_t *vnode = g_vfs_nodes[index];
        vfs_backend_node_t *backend = vnode != 0 ?
                                      (vfs_backend_node_t *)vnode->fs_private : 0;
        /* Registry eviction may lag behind unlink commit. Old handles retain
         * the detached vnode, but a new open must not reuse its page cache. */
        if (backend != 0 && backend->fat32_node &&
            !__atomic_load_n(&backend->detached, __ATOMIC_ACQUIRE) &&
            vfs_fat_relative_path_equal(relative, relative_length,
                                         mount, backend->path)) return vnode;
    }
    return 0;
}

static bool vfs_path_range_equals(const char *left, size_t left_length,
                                  const char *right, size_t right_length) {
    if (left == 0 || right == 0 || left_length != right_length) return false;
    return memcmp(left, right, left_length) == 0;
}

/* Memory-backed /tmp must not fail after a fixed number of profile files.
 * All registry reads and writes, including growth, hold g_vfs_lock. */
static uint32_t vfs_free_node_slot_locked(void) {
    for (uint32_t index = g_vfs_next_free_node; index < g_vfs_node_capacity; ++index) {
        if (g_vfs_nodes[index] == 0) {
            g_vfs_next_free_node = index;
            return index;
        }
    }
    g_vfs_next_free_node = g_vfs_node_capacity;
    if (g_vfs_node_capacity > UINT32_MAX / 2U) return UINT32_MAX;
    uint32_t old_capacity = g_vfs_node_capacity;
    uint32_t new_capacity = old_capacity * 2U;
    vnode_t **replacement = (vnode_t **)kzalloc(
        (size_t)new_capacity * sizeof(*replacement), 0);
    if (replacement == 0) return UINT32_MAX;
    memcpy(replacement, g_vfs_nodes, (size_t)old_capacity * sizeof(*replacement));
    vnode_t **old_nodes = g_vfs_nodes;
    g_vfs_nodes = replacement;
    g_vfs_node_capacity = new_capacity;
    if (old_nodes != g_vfs_initial_nodes) kfree(old_nodes);
    return old_capacity;
}

/* Caller holds the namespace lock. The hint is a lower bound, not a
 * reservation: repeated free-slot queries must return the same slot until
 * publication. All insertion/removal paths update it here. */
static void vfs_set_node_slot_locked(uint32_t index, vnode_t *vnode) {
    g_vfs_nodes[index] = vnode;
    if (vnode == 0) {
        if (index < g_vfs_next_free_node) g_vfs_next_free_node = index;
    } else if (index == g_vfs_next_free_node) {
        g_vfs_next_free_node = index + 1U;
    }
}

/* /tmp is volatile process state, not persistent user data.  Keep the whole
 * namespace off the FAT-backed root: Chromium uses both its profile and
 * anonymous/shared-memory backing files here, and FAT allocation can otherwise
 * synchronously scan the free-cluster chain on the calling UI thread. */
static bool vfs_volatile_path(const char *path) {
    static const char prefix[] = "/tmp";
    const size_t prefix_length = sizeof(prefix) - 1U;
    if (path == 0) return false;
    for (size_t i = 0U; i < prefix_length; ++i) {
        if (path[i] != prefix[i]) return false;
    }
    char boundary = path[prefix_length];
    return boundary == '\0' || boundary == '/';
}

static bool vfs_mount_matches(const vfs_fat32_mount_t *mount,
                              const char *path, size_t path_length,
                              size_t prefix_length) {
    if (mount == 0 || !mount->mounted || mount->filesystem == 0 ||
        path == 0 || path_length == 0U || path[0] != '/') return false;
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
    if (vfs_volatile_path(path)) {
        relative[0] = 0;
        return 0;
    }
    for (uint32_t index = 0U; index < VFS_FAT32_MOUNT_LIMIT; ++index) {
        vfs_fat32_mount_t *candidate = &g_fat32_mounts[index];
        if (!candidate->mounted || candidate->filesystem == 0) continue;
        size_t prefix_length = vfs_string_length(candidate->prefix);
        if (prefix_length <= selected_length ||
            !vfs_mount_matches(candidate, path, path_length, prefix_length)) continue;
        selected = candidate;
        selected_length = prefix_length;
    }
    if (selected == 0) return 0;

    if (path_length == selected_length) {
        relative[0] = 0;
    } else {
        size_t offset = selected_length == 1U ? 1U : selected_length + 1U;
        size_t length = path_length - offset;
        if (length >= VFS_PATH_LIMIT) return 0;
        for (size_t index = 0U; index < length; ++index) {
            relative[index] = path[offset + index];
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
    return mount != 0 && fat32_stat_path(mount->filesystem, relative, info);
}

kstatus_t vfs_mount_fat32(const char *prefix, OS_FAT32 *filesystem) {
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
            g_fat32_mounts[index].prefix[length] == 0 &&
            vfs_path_range_equals(g_fat32_mounts[index].prefix,
                                  length, prefix, length)) {
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

kstatus_t vfs_sync_all(void) {
    kstatus_t result = K_OK;
    vfs_initialize();
    vfs_lock();
    uint32_t limit = g_vfs_node_capacity;
    vfs_unlock();
    for (uint32_t i = 0U; i < limit; ++i) {
        vfs_lock();
        vnode_t *vnode = g_vfs_nodes[i];
        if (vnode != 0) object_get(vnode);
        vfs_unlock();
        if (vnode == 0) continue;
        vfs_vnode_lock(vnode);
        kstatus_t status = vfs_page_cache_flush(vnode);
        vfs_vnode_unlock(vnode);
        object_put(vnode);
        if (status != K_OK) result = status;
    }
    /* Never hold the namespace lock across storage I/O. Mount objects stay
     * alive for the boot lifetime; unmount only detaches their prefix. */
    for (uint32_t i = 0U; i < VFS_FAT32_MOUNT_LIMIT; ++i) {
        vfs_lock();
        OS_FAT32 *filesystem = g_fat32_mounts[i].mounted ?
                              g_fat32_mounts[i].filesystem : 0;
        vfs_unlock();
        if (filesystem != 0 && !fat32_sync(filesystem)) result = K_EIO;
    }
    return result;
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
    /* /tmp is a volatile compatibility directory even when the root image
     * does not carry a persistent directory entry for it. */
    if (path_length == 4U && vfs_path_range_equals(path, path_length,
                                                   "/tmp", 4U)) {
        return true;
    }
    char relative[VFS_PATH_LIMIT];
    os_file_info_t mounted_info;
    if (vfs_fat32_stat_absolute(path, &mounted_info, relative)) {
        return mounted_info.type == OS_FILE_TYPE_DIRECTORY;
    }
    for (uint32_t node_index = 0U; node_index < g_vfs_node_capacity; ++node_index) {
        vnode_t *vnode = g_vfs_nodes[node_index];
        vfs_backend_node_t *backend = vnode != 0 ?
                                      (vfs_backend_node_t *)vnode->fs_private : 0;
        if (backend == 0) continue;
        size_t matched = 0U;
        while (matched < path_length && matched < VFS_PATH_LIMIT &&
               backend->path[matched] != 0 &&
               backend->path[matched] == path[matched]) ++matched;
        if (matched != path_length) continue;
        /* No byte beyond the bounded path participates in an exact match. */
        if (path_length == VFS_PATH_LIMIT || backend->path[path_length] == 0)
            return backend->directory;
        if (backend->path[path_length] == '/') return true;
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
    size_t length = 0U;
    size_t start = 0U;
    if (path != 0) {
        while (length < VFS_PATH_LIMIT && path[length] != 0) {
            if (path[length] == '/') start = length + 1U;
            ++length;
        }
    }
    if (length == 1U && path[0] == '/') {
        name[0] = '/';
        name[1] = 0;
        return;
    }
    size_t count = length - start;
    if (count >= OS_FILE_NAME_MAX) count = OS_FILE_NAME_MAX - 1U;
    for (size_t index = 0U; index < count; ++index) name[index] = path[start + index];
    name[count] = 0;
}

static void vfs_fill_info(const char *path, const vnode_t *vnode,
                          const vfs_backend_node_t *backend,
                          os_file_info_t *out) {
    *out = (os_file_info_t){0};
    out->type = backend != 0 && (backend->mode & 0170000U) == 0120000U ?
        OS_FILE_TYPE_SYMLINK :
        (backend != 0 && backend->directory ? OS_FILE_TYPE_DIRECTORY :
                                             OS_FILE_TYPE_REGULAR);
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

kstatus_t vfs_directory_path_kernel(file_t *file, char *path, size_t capacity) {
    if (file == 0 || file->object.type != KOBJECT_TYPE_FILE || file->vnode == 0)
        return K_EBADF;
    if (path == 0 || capacity == 0U) return K_EINVAL;
    vfs_backend_node_t *backend = file->vnode->fs_private;
    if (backend == 0) return K_EIO;
    vfs_initialize();
    vfs_lock();
    kstatus_t status = K_OK;
    vnode_t *current = backend->fat32_node ? vfs_find_fat_node_locked(backend->path) :
                                           vfs_find_node_locked(backend->path);
    if (!backend->directory) status = K_ENOTDIR;
    else if (backend->detached || current != file->vnode) status = K_ENOENT;
    else {
        size_t length = vfs_string_length(backend->path);
        if (length >= capacity) status = K_ENAMETOOLONG;
        else {
            for (size_t index = 0U; index <= length; ++index)
                path[index] = backend->path[index];
        }
    }
    vfs_unlock();
    return status;
}

static bool vfs_path_is_subtree(const char *path, size_t path_length,
                                const char *root, size_t root_length) {
    return (path_length == root_length &&
            vfs_path_range_equals(path, path_length, root, root_length)) ||
           (path_length > root_length && path[root_length] == '/' &&
            vfs_path_range_equals(path, root_length, root, root_length));
}

/* Registry paths are fixed-size arrays; the root is already validated. */
static bool vfs_path_matches_subtree_prefix(const char *path, const char *root,
                                            size_t root_length) {
    return root_length < VFS_PATH_LIMIT &&
           (path[root_length] == 0 || path[root_length] == '/') &&
           vfs_path_range_equals(path, root_length, root, root_length);
}

static kstatus_t vfs_rekey_cached_subtree_locked(const char *old_path,
                                            size_t old_length,
                                            const char *new_path,
                                            size_t new_length, bool publish) {
    char old_relative[VFS_PATH_LIMIT];
    vfs_fat32_mount_t *old_mount = vfs_fat32_mount_for_path(old_path, old_relative);
    size_t relative_length = old_mount != 0 ? vfs_string_length(old_relative) : 0U;
    /* Validate the entire set before publishing any path. The namespace
     * lock keeps both passes consistent, including mount aliases. */
    for (unsigned pass = 0U; pass < (publish ? 2U : 1U); ++pass) {
    for (uint32_t index = 0U; index < g_vfs_node_capacity; ++index) {
        vnode_t *candidate = g_vfs_nodes[index];
        vfs_backend_node_t *backend = candidate != 0 ?
            (vfs_backend_node_t *)candidate->fs_private : 0;
        size_t candidate_length;
        size_t suffix_length;
        char replacement[VFS_PATH_LIMIT];
        char candidate_relative[VFS_PATH_LIMIT];
        const char *suffix;
        if (old_mount != 0) {
            if (backend == 0 || !backend->fat32_node ||
                __atomic_load_n(&backend->detached, __ATOMIC_ACQUIRE)) continue;
            vfs_fat32_mount_t *candidate_mount =
                vfs_fat32_mount_for_path(backend->path, candidate_relative);
            if (candidate_mount == 0 ||
                candidate_mount->filesystem != old_mount->filesystem) continue;
            size_t matched = 0U;
            while (matched < relative_length) {
                char a = old_relative[matched];
                char b = candidate_relative[matched];
                if (b == 0) break;
                if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
                if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
                if (a != b) break;
                ++matched;
            }
            if (matched != relative_length ||
                (candidate_relative[matched] != 0 &&
                 candidate_relative[matched] != '/')) continue;
            suffix = candidate_relative + matched;
            suffix_length = vfs_string_length(suffix);
        } else {
            if (backend == 0 ||
                !vfs_path_matches_subtree_prefix(backend->path, old_path, old_length)) continue;
            candidate_length = vfs_string_length(backend->path);
            suffix_length = candidate_length - old_length;
            suffix = backend->path + old_length;
        }
        if (new_length >= VFS_PATH_LIMIT ||
            suffix_length >= VFS_PATH_LIMIT - new_length) return K_ENAMETOOLONG;
        if (pass == 0U) continue;
        __builtin_memcpy(replacement, new_path, new_length);
        __builtin_memcpy(replacement + new_length, suffix, suffix_length);
        replacement[new_length + suffix_length] = '\0';
        __builtin_memcpy(backend->path, replacement,
                         new_length + suffix_length + 1U);
    }
    }
    return K_OK;
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
        os_file_info_t info;
        return vfs_stat_kernel(old_path, &info);
    }

    old_mount = vfs_fat32_mount_for_path(old_path, old_relative);
    new_mount = vfs_fat32_mount_for_path(new_path, new_relative);
    old_mounted = old_mount != 0;
    new_mounted = new_mount != 0;
    if (old_mounted || new_mounted) {
        if (!old_mounted || !new_mounted || old_mount != new_mount ||
            old_relative[0] == 0 ||
            new_relative[0] == 0) return K_EXDEV;
        vfs_initialize();
        vfs_lock();
        kstatus_t rekey_status = vfs_rekey_cached_subtree_locked(
            old_path, old_length, new_path, new_length, false);
        vfs_unlock();
        if (rekey_status != K_OK) return rekey_status;
        if (!fat32_rename_path(old_mount->filesystem,
                                      old_relative, new_relative)) {
            return K_EIO;
        }
        /* FAT keeps the inode alive across a rename.  Rekey cached vnodes so
         * already-open files continue to use the new path for later I/O and
         * fstat, while fresh opens do not retain the old cache key. */
        vfs_initialize();
        vfs_lock();
        rekey_status = vfs_rekey_cached_subtree_locked(old_path, old_length,
                                                      new_path, new_length, true);
        vfs_unlock();
        return rekey_status;
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
    vnode_t *replaced = vfs_find_node_locked(new_path);
    vfs_backend_node_t *replaced_backend = replaced != 0 ?
        (vfs_backend_node_t *)replaced->fs_private : 0;
    if (replaced_backend != 0) {
        if (!replaced_backend->mutable_node) {
            vfs_unlock();
            return K_EACCES;
        }
        if (source_backend->directory != replaced_backend->directory) {
            vfs_unlock();
            return replaced_backend->directory ? K_EISDIR : K_ENOTDIR;
        }
        if (replaced_backend->directory) {
            for (uint32_t index = 0U; index < g_vfs_node_capacity; ++index) {
                vnode_t *candidate = g_vfs_nodes[index];
                vfs_backend_node_t *backend = candidate != 0 ?
                    (vfs_backend_node_t *)candidate->fs_private : 0;
                if (backend != 0 && candidate != replaced &&
                    vfs_path_matches_subtree_prefix(backend->path, new_path, new_length)) {
                    vfs_unlock();
                    return K_ENOTEMPTY;
                }
            }
        }
    } else if (vfs_directory_exists_locked(new_path, new_length)) {
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
    for (uint32_t index = 0U; index < g_vfs_node_capacity; ++index) {
        vnode_t *candidate = g_vfs_nodes[index];
        vfs_backend_node_t *backend = candidate != 0 ?
            (vfs_backend_node_t *)candidate->fs_private : 0;
        size_t candidate_length;
        size_t suffix_length;
        if (backend == 0 ||
            !vfs_path_matches_subtree_prefix(backend->path, old_path, old_length)) continue;
        candidate_length = vfs_string_length(backend->path);
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
    /* Publish replacement and source rekey under the same namespace lock.
     * Existing opens retain their vnode references and the old contents. */
    if (replaced != 0) {
        for (uint32_t index = 0U; index < g_vfs_node_capacity; ++index) {
            if (g_vfs_nodes[index] == replaced) {
                vfs_set_node_slot_locked(index, 0);
                break;
            }
        }
    }
    for (uint32_t index = 0U; index < g_vfs_node_capacity; ++index) {
        vnode_t *candidate = g_vfs_nodes[index];
        vfs_backend_node_t *backend = candidate != 0 ?
            (vfs_backend_node_t *)candidate->fs_private : 0;
        size_t candidate_length;
        size_t suffix_length;
        char replacement[VFS_PATH_LIMIT];
        if (backend == 0 ||
            !vfs_path_matches_subtree_prefix(backend->path, old_path, old_length)) continue;
        candidate_length = vfs_string_length(backend->path);
        suffix_length = candidate_length - old_length;
        __builtin_memcpy(replacement, new_path, new_length);
        __builtin_memcpy(replacement + new_length, backend->path + old_length,
                         suffix_length);
        replacement[new_length + suffix_length] = '\0';
        __builtin_memcpy(backend->path, replacement,
                         new_length + suffix_length + 1U);
    }
    vfs_unlock();
    if (replaced != 0) object_put(replaced);
    return K_OK;
}

static size_t vfs_file_name_length(const char *name) {
    size_t length = 0U;
    while (length < OS_FILE_NAME_MAX && name[length] != 0) ++length;
    return length;
}

static bool vfs_append_entry(os_file_info_t entries[VFS_DIRECTORY_ENTRY_LIMIT],
                             uint32_t *entry_count,
                             const os_file_info_t *entry) {
    if (entries == 0 || entry_count == 0 || entry == 0 || entry->name[0] == 0) {
        return false;
    }
    size_t name_length = vfs_file_name_length(entry->name);
    if (name_length >= OS_FILE_NAME_MAX) return false;
    for (uint32_t index = 0U; index < *entry_count; ++index) {
        /* The incoming length is bounded by the fixed name array. Check
         * the candidate terminator there instead of rescanning its length. */
        if (entries[index].name[name_length] != 0 ||
            !vfs_path_range_equals(entries[index].name, name_length,
                                   entry->name, name_length)) {
            continue;
        }
        if (entry->type == OS_FILE_TYPE_DIRECTORY) entries[index] = *entry;
        return true;
    }
    if (*entry_count >= VFS_DIRECTORY_ENTRY_LIMIT) return false;
    entries[*entry_count] = *entry;
    ++*entry_count;
    return true;
}

typedef struct vfs_directory_collection {
    os_file_info_t *entries;
    uint32_t *count;
    bool failed;
} vfs_directory_collection_t;

static BOOLEAN vfs_collect_fat_directory_entry(VOID *context, const os_file_info_t *entry) {
    vfs_directory_collection_t *collection = context;
    if (!vfs_append_entry(collection->entries, collection->count, entry)) {
        collection->failed = true;
        return 0;
    }
    return 1;
}

/* Use one direct-child rule for both synthetic directory lookup and listing. */
static const char *vfs_mount_child_name(const vfs_fat32_mount_t *mount,
                                        const char *path, size_t path_length,
                                        size_t *name_length) {
    if (!mount->mounted) return 0;
    size_t prefix_length = vfs_string_length(mount->prefix);
    if (prefix_length <= 1U) return 0;
    size_t leaf_start = prefix_length;
    while (leaf_start != 0U && mount->prefix[leaf_start - 1U] != '/') --leaf_start;
    size_t parent_length = leaf_start <= 1U ? 1U : leaf_start - 1U;
    if (parent_length != path_length ||
        !vfs_path_range_equals(path, path_length, mount->prefix, parent_length)) return 0;
    *name_length = prefix_length - leaf_start;
    return mount->prefix + leaf_start;
}

static bool vfs_mount_parent_exists(const char *path, size_t path_length) {
    for (uint32_t index = 0U; index < VFS_FAT32_MOUNT_LIMIT; ++index) {
        vfs_fat32_mount_t *mount = &g_fat32_mounts[index];
        size_t name_length;
        if (vfs_mount_child_name(mount, path, path_length, &name_length) != 0) return true;
    }
    return false;
}

static bool vfs_append_mount_children(const char *path, size_t path_length,
                                      os_file_info_t entries[VFS_DIRECTORY_ENTRY_LIMIT],
                                      uint32_t *entry_count,
                                      bool *directory_exists) {
    for (uint32_t index = 0U; index < VFS_FAT32_MOUNT_LIMIT; ++index) {
        vfs_fat32_mount_t *mount = &g_fat32_mounts[index];
        size_t name_length;
        const char *name = vfs_mount_child_name(mount, path, path_length, &name_length);
        if (name == 0) continue;
        if (directory_exists != 0) *directory_exists = true;
        if (name_length >= OS_FILE_NAME_MAX) return false;
        os_file_info_t entry = {0};
        entry.type = OS_FILE_TYPE_DIRECTORY;
        entry.mode = 0040755U;
        memcpy(entry.name, name, name_length);
        if (!vfs_append_entry(entries, entry_count, &entry)) return false;
    }
    return true;
}

/* The current VFS stores files by absolute path.  Derive directory entries
 * from those paths here so every user of the VFS sees the same namespace. */
static kstatus_t vfs_enumerate_kernel_batch(const char *path, uint32_t index,
                                            os_file_info_t *out, uint32_t capacity,
                                            uint32_t *count) {
    os_file_info_t entries[VFS_DIRECTORY_ENTRY_LIMIT];
    size_t directory_length;
    uint32_t entry_count = 0U;
    bool directory_exists = false;
    bool exact_regular_file = false;
    char relative[VFS_PATH_LIMIT];
    vfs_fat32_mount_t *mount;
    os_file_info_t mounted_info;

    if (count == 0) return K_EINVAL;
    *count = 0U;
    if (path == 0 || out == 0 || capacity == 0U || path[0] != '/') return K_EINVAL;
    directory_length = vfs_string_length(path);
    if (directory_length == 0U || directory_length >= VFS_PATH_LIMIT) return K_EINVAL;
    while (directory_length > 1U && path[directory_length - 1U] == '/') --directory_length;
    /* Only [0, entry_count) is read; append publishes complete entries. */

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
                fat32_stat_path(mount->filesystem, relative,
                                        &mounted_info);
        }
    }
    if (mounted_directory) {
        if (mounted_info.type != OS_FILE_TYPE_DIRECTORY) return K_EINVAL;
        directory_exists = true;
        vfs_directory_collection_t collection = {entries, &entry_count, false};
        fat32_visit_directory(mount->filesystem, relative,
                                     VFS_DIRECTORY_ENTRY_LIMIT,
                                     vfs_collect_fat_directory_entry, &collection);
        if (collection.failed) return K_ENOMEM;
    }
    if (!vfs_append_mount_children(path, directory_length, entries, &entry_count,
                                   &directory_exists)) {
        return K_ENOMEM;
    }
    if (directory_length == 1U) directory_exists = true;

    vfs_initialize();
    vfs_lock();
    for (uint32_t node_index = 0U; node_index < g_vfs_node_capacity; ++node_index) {
        vnode_t *vnode = g_vfs_nodes[node_index];
        vfs_backend_node_t *backend = vnode != 0 ?
                                      (vfs_backend_node_t *)vnode->fs_private : 0;
        size_t remainder_index;
        const char *remainder;
        if (backend == 0) continue;
        size_t matched = 0U;
        while (matched < directory_length && backend->path[matched] != 0 &&
               backend->path[matched] == path[matched]) ++matched;
        if (matched != directory_length) continue;
        if (backend->path[directory_length] == 0) {
            if (backend->directory) directory_exists = true;
            else exact_regular_file = true;
            continue;
        }
        if (directory_length != 1U && backend->path[directory_length] != '/') continue;
        remainder_index = directory_length == 1U ? 1U : directory_length + 1U;
        if (remainder_index >= VFS_PATH_LIMIT || backend->path[remainder_index] == 0) continue;
        directory_exists = true;
        remainder = backend->path + remainder_index;
        size_t name_length = 0U;
        while (name_length < OS_FILE_NAME_MAX &&
               remainder_index + name_length < VFS_PATH_LIMIT &&
               remainder[name_length] != '\0' && remainder[name_length] != '/') ++name_length;
        if (name_length == 0U || name_length >= OS_FILE_NAME_MAX ||
            remainder_index + name_length >= VFS_PATH_LIMIT) {
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
    uint32_t copied = entry_count - index;
    if (copied > capacity) copied = capacity;
    memcpy(out, entries + index, (size_t)copied * sizeof(*out));
    *count = copied;
    return K_OK;
}

kstatus_t vfs_enumerate_kernel(const char *path, uint32_t index,
                               os_file_info_t *out) {
    uint32_t count;
    return vfs_enumerate_kernel_batch(path, index, out, 1U, &count);
}

kstatus_t vfs_enumerate_file(file_t *file, uint32_t index,
                             os_file_info_t *out) {
    vfs_backend_node_t *backend;
    if (file == 0 || file->object.type != KOBJECT_TYPE_FILE ||
        file->vnode == 0 || out == 0) return K_EBADF;
    backend = (vfs_backend_node_t *)file->vnode->fs_private;
    if (backend == 0 || !backend->directory) return K_ENOTDIR;
    return vfs_enumerate_kernel(backend->path, index, out);
}

kstatus_t vfs_enumerate_file_batch(file_t *file, uint32_t index,
                                   os_file_info_t *out, uint32_t capacity,
                                   uint32_t *count) {
    if (count == 0) return K_EINVAL;
    *count = 0U;
    if (file == 0 || file->object.type != KOBJECT_TYPE_FILE ||
        file->vnode == 0 || out == 0) return K_EBADF;
    vfs_backend_node_t *backend = file->vnode->fs_private;
    if (backend == 0 || !backend->directory) return K_ENOTDIR;
    return vfs_enumerate_kernel_batch(backend->path, index, out, capacity, count);
}

static vnode_t *vfs_create_vnode_object(vfs_backend_node_t *backend) {
    vnode_t *vnode;
    uint64_t inode_id;
    if (backend == 0) return 0;
    vnode = (vnode_t *)kzalloc(sizeof(*vnode), 0);
    if (vnode == 0) return 0;
    inode_id = atomic_fetch_add_explicit(&g_vfs_next_inode_id, 1U,
                                         memory_order_relaxed) + 1U;
    if (inode_id == 0U) {
        kfree(vnode);
        return 0;
    }
    refcount_init(&vnode->object.refs, 1U);
    vnode->object.type = KOBJECT_TYPE_VNODE;
    vnode->object.flags = 0;
    vnode->object.ops = &g_vnode_object_ops;
    vnode->inode_id = inode_id;
    vnode->mode = backend->mode;
    vnode->flags = 0;
    vnode->size = backend->size;
    vnode->ops = 0;
    vnode->fs_private = backend;
    atomic_init(&vnode->lock.state, 0U);
    vnode->page_cache = 0;
    wait_queue_init(&vnode->record_lock_waitq);
    atomic_init(&vnode->record_lock_sequence, 0U);
    return vnode;
}

/* Release one idle native handle under pressure, retaining its VFS page
 * cache. A registry-only inode has no live file or mapping references. */
static bool vfs_release_idle_fat_handle(OS_FAT32 *filesystem) {
    for (uint32_t index = 0U;; ++index) {
        vnode_t *candidate = 0;
        vfs_lock();
        if (index >= g_vfs_node_capacity) {
            vfs_unlock();
            return false;
        }
        candidate = g_vfs_nodes[index];
        if (candidate != 0 &&
            atomic_load_explicit(&candidate->object.refs.value, memory_order_acquire) == 1U) {
            object_get(candidate);
        } else {
            candidate = 0;
        }
        vfs_unlock();
        if (candidate == 0) continue;
        vfs_backend_node_t *backend = (vfs_backend_node_t *)candidate->fs_private;
        bool released = false;
        if (backend != 0) {
            vfs_backend_lock(backend);
            /* Recheck after pinning: a concurrent open may now own it. */
            if (atomic_load_explicit(&candidate->object.refs.value, memory_order_acquire) == 2U &&
                backend->fat_file != 0 && !backend->detached &&
                backend->fat_file->FileSystem == filesystem &&
                fat32_close(backend->fat_file)) {
                backend->fat_file = 0;
                released = true;
            }
            vfs_backend_unlock(backend);
        }
        object_put(candidate);
        if (released) return true;
    }
}

static kstatus_t vfs_create_fat32_vnode(const char *path,
                                        bool keep_fat_file,
                                        const os_file_info_t *known_info,
                                        vnode_t **out) {
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
    } else if (known_info != 0) {
        /* The open path already queried this entry; avoid a second directory
         * traversal. Callers creating a new entry must pass no snapshot. */
        info = *known_info;
    } else if (!fat32_stat_path(mount->filesystem, relative, &info) ||
               info.type == OS_FILE_TYPE_UNKNOWN) {
        if (!vfs_mount_parent_exists(path, vfs_string_length(path))) {
            return K_ENOENT;
        }
        info = (os_file_info_t){
            .type = OS_FILE_TYPE_DIRECTORY, .mode = 0040755U
        };
    }
    /* Cached-open fast path: no temporary allocation or native FAT handle.
     * Still recheck after allocation below to resolve concurrent misses. */
    vfs_initialize();
    vfs_lock();
    vnode_t *cached = vfs_find_fat_node_locked(path);
    if (cached != 0) {
        object_get(cached);
        vfs_unlock();
        *out = cached;
        return K_OK;
    }
    vfs_unlock();
    /* Cached-open fast path end. */
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
    backend->fat_file = 0;
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
    if (keep_fat_file && !backend->directory &&
        !fat32_open(mount->filesystem, relative,
                           &backend->fat_file)) {
        if (!vfs_release_idle_fat_handle(mount->filesystem) ||
            !fat32_open(mount->filesystem, relative, &backend->fat_file)) {
            object_put(vnode);
            return K_EIO;
        }
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
        /* The canonical inode already supports coherent direct read/write.
         * A writable open must not discard its handle and seek caches. */
        object_put(vnode);
        *out = existing;
        return K_OK;
    }
    uint32_t slot = vfs_free_node_slot_locked();
    if (slot == UINT32_MAX) {
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
    vfs_set_node_slot_locked(slot, vnode);
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
    uint32_t slot = vfs_free_node_slot_locked();
    if (slot == UINT32_MAX) {
        vfs_unlock();
        object_put(vnode);
        return K_ENOMEM;
    }
    vfs_set_node_slot_locked(slot, vnode);
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
        if (fat32_stat_path(mount->filesystem, relative, &existing_info)) {
            return K_EEXIST;
        }
        if (!fat32_create_path(mount->filesystem, relative, directory)) {
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

/* Immutable link contents are published together with their namespace entry.
 * Parent validation and collision detection share the namespace lock: a
 * failed or racing creation never exposes a partially initialized link. */
kstatus_t vfs_symlink_kernel(const char *target, const char *path) {
    char relative[VFS_PATH_LIMIT];
    size_t length = vfs_string_length(path);
    size_t target_length = vfs_string_length(target);
    if (path == 0 || target == 0 || path[0] != '/') return K_EINVAL;
    if (target_length == 0U) return K_ENOENT;
    if (length >= VFS_PATH_LIMIT || target_length >= VFS_PATH_LIMIT)
        return K_ENAMETOOLONG;
    if (length <= 1U || path[length - 1U] == '/') return K_EINVAL;
    /* FAT has no native symlink representation. Do not publish a volatile
     * shadow inode over persistent files; /tmp is already memory-backed. */
    if (vfs_fat32_mount_for_path(path, relative) != 0) return K_EPERM;
    vfs_backend_node_t *backend = kzalloc(sizeof(*backend), 0);
    if (backend == 0) return K_ENOMEM;
    backend->data = kzalloc(target_length + 1U, 0);
    if (backend->data == 0) { kfree(backend); return K_ENOMEM; }
    memcpy(backend->path, path, length + 1U);
    memcpy(backend->data, target, target_length);
    backend->size = target_length;
    backend->data_capacity = target_length + 1U;
    backend->mode = 0120777U;
    backend->mutable_node = true;
    atomic_init(&backend->io_lock.state, 0U);
    vnode_t *vnode = vfs_create_vnode_object(backend);
    if (vnode == 0) {
        kfree(backend->data); kfree(backend); return K_ENOMEM;
    }
    vfs_initialize();
    vfs_lock();
    kstatus_t status;
    if (vfs_find_node_locked(path) != 0) status = K_EEXIST;
    else if (!vfs_parent_directory_exists_locked(path, length)) status = K_ENOENT;
    else {
        uint32_t slot = vfs_free_node_slot_locked();
        status = slot == UINT32_MAX ? K_ENOMEM : K_OK;
        if (status == K_OK) vfs_set_node_slot_locked(slot, vnode);
    }
    vfs_unlock();
    if (status != K_OK) object_put(vnode);
    return status;
}

kstatus_t vfs_readlink_kernel(const char *path, char *buffer, size_t capacity,
                             size_t *bytes) {
    if (path == 0 || path[0] != '/' || buffer == 0 || bytes == 0 || capacity == 0U)
        return K_EINVAL;
    *bytes = 0U;
    if (vfs_string_length(path) >= VFS_PATH_LIMIT) return K_ENAMETOOLONG;
    vfs_initialize();
    vfs_lock();
    vnode_t *vnode = vfs_find_node_locked(path);
    if (vnode == 0) {
        vfs_unlock();
        os_file_info_t info;
        kstatus_t status = vfs_stat_kernel(path, &info);
        return status == K_OK ? K_EINVAL : status;
    }
    vfs_backend_node_t *backend = vnode->fs_private;
    if (backend == 0 || (backend->mode & 0170000U) != 0120000U) {
        vfs_unlock(); return K_EINVAL;
    }
    if (backend->data == 0 || backend->size == 0U || backend->size >= VFS_PATH_LIMIT) {
        vfs_unlock(); return K_EIO;
    }
    size_t count = backend->size < capacity ? (size_t)backend->size : capacity;
    memcpy(buffer, backend->data, count);
    *bytes = count; // readlink deliberately does not append a terminator.
    vfs_unlock();
    return K_OK;
}

/* Resolve components in order: collapse '..' only after expanding a preceding
 * link. Metadata/readlink operations take their own short namespace locks; no
 * backing-store lookup or allocation runs while retaining the namespace lock. */
kstatus_t vfs_resolve_path_kernel(const char *path, bool follow_final,
                                bool allow_missing_leaf, char output[256]) {
    char pending[VFS_PATH_LIMIT], candidate[VFS_PATH_LIMIT];
    char target[VFS_PATH_LIMIT], replacement[VFS_PATH_LIMIT];
    size_t length = vfs_string_length(path);
    if (path == 0 || output == 0 || path[0] != '/') return K_EINVAL;
    if (length >= VFS_PATH_LIMIT) return K_ENAMETOOLONG;
    bool trailing = length > 0U && path[length - 1U] == '/';
    memcpy(pending, path, length + 1U);
    output[0] = '/'; output[1] = 0;
    size_t prefix = 1U, cursor = 0U;
    unsigned links = 0U;
    for (;;) {
        while (pending[cursor] == '/') ++cursor;
        if (pending[cursor] == 0) return K_OK;
        size_t start = cursor;
        while (pending[cursor] != 0 && pending[cursor] != '/') ++cursor;
        size_t component = cursor - start;
        bool separator = pending[cursor] == '/';
        size_t tail = cursor;
        while (pending[tail] == '/') ++tail;
        bool last = pending[tail] == 0;
        if (component == 1U && pending[start] == '.') continue;
        if (component == 2U && pending[start] == '.' && pending[start + 1U] == '.') {
            while (prefix > 1U && output[prefix - 1U] != '/') --prefix;
            if (prefix > 1U) --prefix;
            output[prefix] = 0;
            continue;
        }
        size_t base = prefix + (prefix > 1U ? 1U : 0U);
        if (base + component >= VFS_PATH_LIMIT) return K_ENAMETOOLONG;
        memcpy(candidate, output, prefix);
        if (prefix > 1U) candidate[prefix] = '/';
        memcpy(candidate + base, pending + start, component);
        candidate[base + component] = 0;
        os_file_info_t info;
        kstatus_t status = vfs_stat_kernel(candidate, &info);
        if (status == K_ENOENT && last && allow_missing_leaf && !separator && !trailing) {
            memcpy(output, candidate, base + component + 1U);
            return K_OK;
        }
        if (status != K_OK) return status;
        bool directory_required = !last || separator || trailing;
        if (info.type == OS_FILE_TYPE_SYMLINK &&
            (!last || follow_final || directory_required)) {
            if (++links > 40U) return K_ELOOP;
            size_t bytes = 0U;
            status = vfs_readlink_kernel(candidate, target, sizeof(target) - 1U, &bytes);
            if (status != K_OK) return status;
            if (bytes == 0U || bytes >= VFS_PATH_LIMIT) return K_EIO;
            target[bytes] = 0;
            size_t remaining = vfs_string_length(pending + cursor);
            if (bytes + remaining >= VFS_PATH_LIMIT) return K_ENAMETOOLONG;
            memcpy(replacement, target, bytes);
            memcpy(replacement + bytes, pending + cursor, remaining + 1U);
            memcpy(pending, replacement, bytes + remaining + 1U);
            cursor = 0U;
            if (target[0] == '/') { prefix = 1U; output[1] = 0; }
            continue;
        }
        if (directory_required && info.type != OS_FILE_TYPE_DIRECTORY) return K_ENOTDIR;
        prefix = base + component;
        memcpy(output, candidate, prefix + 1U);
    }
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
        bool found = fat32_stat_path(mount->filesystem, relative, out);
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

kstatus_t vfs_remove_typed_kernel(const char *path, int directory) {
    size_t path_length;
    vnode_t *victim = 0;
    char relative[VFS_PATH_LIMIT];
    vfs_fat32_mount_t *mount;
    os_file_info_t mounted_info;
    if (path == 0 || path[0] != '/' || directory < -1 || directory > 1)
        return K_EINVAL;
    path_length = vfs_string_length(path);
    if (path_length <= 1U || path_length >= VFS_PATH_LIMIT) return K_EINVAL;
    while (path_length > 1U && path[path_length - 1U] == '/') --path_length;
    if (path_length <= 1U) return K_EINVAL;
    mount = vfs_fat32_mount_for_path(path, relative);
    if (mount != 0) {
        vnode_t *mounted_victim = 0;
        vfs_backend_node_t *mounted_backend = 0;
        if (relative[0] == 0 ||
            !fat32_stat_path(mount->filesystem, relative,
                                    &mounted_info)) {
            return K_ENOENT;
        }
        vfs_initialize();
        if (directory >= 0 &&
            ((mounted_info.type == OS_FILE_TYPE_DIRECTORY) != (directory != 0)))
            return directory != 0 ? K_ENOTDIR : K_EISDIR;
        vfs_lock();
        mounted_victim = vfs_find_fat_node_locked(path);
        if (mounted_victim != 0) {
            object_get(mounted_victim);
            mounted_backend = (vfs_backend_node_t *)mounted_victim->fs_private;
        }
        vfs_unlock();
        if (mounted_backend != 0) {
            kstatus_t status = vfs_fat32_backend_prepare_unlink(mounted_backend);
            if (status != K_OK) {
                object_put(mounted_victim);
                return status;
            }
        }
        if (mounted_backend != 0) {
            vfs_backend_lock(mounted_backend);
            if (!mounted_backend->directory &&
                (!mounted_backend->unlink_prepared || mounted_backend->detached ||
                 mounted_backend->size != mounted_backend->snapshot_size ||
                 mounted_backend->content_revision != mounted_backend->snapshot_revision)) {
                vfs_backend_unlock(mounted_backend);
                vfs_fat32_backend_abort_unlink(mounted_backend);
                object_put(mounted_victim);
                return K_EBUSY;
            }
        }
        /* Native FAT refuses unlink while a raw handle is open. The snapshot
         * now owns old-file reads, so release only at the unlink boundary.
         * If deletion fails, subsequent I/O can reopen the unchanged path. */
        if (mounted_backend != 0 && mounted_backend->fat_file != 0) {
            if (!fat32_close(mounted_backend->fat_file)) {
                vfs_backend_unlock(mounted_backend);
                vfs_fat32_backend_abort_unlink(mounted_backend);
                object_put(mounted_victim);
                return K_EIO;
            }
            mounted_backend->fat_file = 0;
        }
        if (!fat32_remove_path_typed(mount->filesystem, relative, directory)) {
            if (mounted_backend != 0) {
                vfs_backend_unlock(mounted_backend);
                vfs_fat32_backend_abort_unlink(mounted_backend);
            }
            if (mounted_victim != 0) object_put(mounted_victim);
            return mounted_info.type == OS_FILE_TYPE_DIRECTORY ? K_ENOTEMPTY : K_EIO;
        }
        if (mounted_backend != 0) {
            vfs_fat32_backend_commit_unlink_locked(mounted_backend);
            vfs_backend_unlock(mounted_backend);
        }
        /* 删除磁盘目录项后同步摘除规范 vnode；仍被打开的 file 引用会让
         * vnode 保持有效。同名重建的 vnode 必须保留。 */
        for (;;) {
            vnode_t *detached = 0;
            vfs_lock();
            /* Evict the pinned inode, never a replacement with the same name.
             * Pointer identity also survives mount changes between passes. */
            for (uint32_t node_index = 0U;
                 node_index < g_vfs_node_capacity; ++node_index) {
                vnode_t *candidate = g_vfs_nodes[node_index];
                if (candidate == 0 || candidate != mounted_victim) continue;
                vfs_set_node_slot_locked(node_index, 0);
                detached = candidate;
                break;
            }
            vfs_unlock();
            if (detached == 0) break;
            object_put(detached);
        }
        if (mounted_victim != 0) object_put(mounted_victim);
        return K_OK;
    }
    vfs_initialize();
    vfs_lock();
    for (uint32_t node_index = 0U; node_index < g_vfs_node_capacity; ++node_index) {
        vnode_t *candidate = g_vfs_nodes[node_index];
        vfs_backend_node_t *backend = candidate != 0 ?
                                      (vfs_backend_node_t *)candidate->fs_private : 0;
        if (backend == 0) continue;
        if (backend->path[path_length] != 0 ||
            !vfs_path_range_equals(backend->path, path_length, path, path_length)) continue;
        if (!backend->mutable_node) {
            vfs_unlock();
            return K_EACCES;
        }
        /* Validate the victim under the same namespace lock as deletion. */
        if (directory >= 0 && backend->directory != (directory != 0)) {
            vfs_unlock();
            return directory != 0 ? K_ENOTDIR : K_EISDIR;
        }
        if (backend->directory) {
            for (uint32_t child_index = 0U; child_index < g_vfs_node_capacity;
                 ++child_index) {
                vnode_t *child = g_vfs_nodes[child_index];
                vfs_backend_node_t *child_backend = child != 0 ?
                    (vfs_backend_node_t *)child->fs_private : 0;
                if (child_backend == 0 || child == candidate) continue;
                if (child_backend->path[path_length] == '/' &&
                    vfs_path_range_equals(child_backend->path, path_length,
                                          path, path_length)) {
                    vfs_unlock();
                    return K_ENOTEMPTY;
                }
            }
        }
        vfs_set_node_slot_locked(node_index, 0);
        victim = candidate;
        break;
    }
    vfs_unlock();
    if (victim == 0) return K_ENOENT;
    object_put(victim);
    return K_OK;
}

kstatus_t vfs_remove_kernel(const char *path) {
    return vfs_remove_typed_kernel(path, -1);
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
         fat32_stat_path(mount->filesystem, relative, &mounted_info));
    bool mounted_parent = mounted_path && !mounted_exists &&
                          vfs_mount_parent_exists(path, vfs_string_length(path));
    if (mounted_path && relative[0] == 0) {
        mounted_info = (os_file_info_t){
            .type = OS_FILE_TYPE_DIRECTORY, .mode = 0040755U
        };
    }
    if (mounted_exists || mounted_parent) {
        kstatus_t mount_status = vfs_create_fat32_vnode(
            path, (flags & VFS_OPEN_WRITE) == 0U,
            mounted_exists ? &mounted_info : 0, &vnode);
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
    /* /tmp is an implicit volatile directory so that callers can create
     * temporary files without a persistent root-image entry.  Materialize
     * its directory vnode on first O_DIRECTORY open; otherwise stat and
     * parent-directory checks say it exists while open("/tmp") returns
     * ENOENT. */
    if (vnode == 0 && (flags & VFS_OPEN_DIRECTORY) != 0U &&
        vfs_string_length(path) == 4U &&
        vfs_path_range_equals(path, 4U, "/tmp", 4U)) {
        kstatus_t directory_status = vfs_create_memory_node(path, 0755U, true);
        if (directory_status != K_OK && directory_status != K_EEXIST) {
            return directory_status;
        }
        vfs_lock();
        vnode = vfs_find_node_locked(path);
        if (vnode != 0) {
            object_get(vnode);
            existed = true;
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
            kstatus_t mount_status = vfs_create_fat32_vnode(
                path, (flags & VFS_OPEN_WRITE) == 0U, 0, &vnode);
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
