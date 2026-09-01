#include "internal.h"
#include <kernel/vm.h>

/* REFACTOR_FS_VFS_PAGE_CACHE_OWNER: file-page cache lifetime and I/O. */

static void vfs_vm_mapping_retain(void *mapping) {
    object_get(mapping);
}

static void vfs_vm_mapping_release(void *mapping) {
    object_put(mapping);
}

static kstatus_t vfs_vm_mapping_page_get(void *mapping, uint64_t page_index,
                                         page_t **out) {
    return vfs_file_page_get((vnode_t *)mapping, page_index, out);
}

static void vfs_vm_mapping_page_mark_dirty(void *mapping, uint64_t page_index) {
    vfs_file_page_mark_dirty((vnode_t *)mapping, page_index);
}

static kstatus_t vfs_vm_mapping_sync(void *mapping, uint64_t first_page,
                                     uint64_t page_count) {
    return vfs_page_cache_sync((vnode_t *)mapping, first_page, page_count);
}

static const vm_file_ops_t g_vfs_vm_file_ops = {
    .retain = vfs_vm_mapping_retain,
    .release = vfs_vm_mapping_release,
    .page_get = vfs_vm_mapping_page_get,
    .page_mark_dirty = vfs_vm_mapping_page_mark_dirty,
    .sync = vfs_vm_mapping_sync,
};

const struct vm_file_ops *vfs_vm_file_ops(void) {
    return &g_vfs_vm_file_ops;
}

void vfs_page_cache_destroy(vnode_t *vnode) {
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

/* Caller holds vnode->lock.  A bounded page range keeps VM_SYNC from flushing
 * unrelated dirty mappings that happen to share the same vnode. */
static kstatus_t vfs_page_cache_flush_locked(vnode_t *vnode,
                                              uint64_t first_page,
                                              uint64_t page_count) {
    uint64_t last_page;
    if (vnode == 0) return K_EINVAL;
    if (page_count == 0U) return K_EINVAL;
    if (page_count == UINT64_MAX) {
        if (first_page != 0U) return K_EINVAL;
        last_page = UINT64_MAX;
    } else {
        if (first_page > UINT64_MAX - (page_count - 1U)) return K_EOVERFLOW;
        last_page = first_page + page_count - 1U;
    }
    vfs_backend_node_t *backend =
        (vfs_backend_node_t *)vnode->fs_private;
    vfs_page_cache_t *cache =
        (vfs_page_cache_t *)vnode->page_cache;
    if (backend == 0 || cache == 0) return K_OK;
    for (vfs_cached_page_t *entry = cache->head; entry != 0;
         entry = entry->next) {
        if (!entry->dirty || entry->index < first_page ||
            entry->index > last_page) continue;
        if (entry->index > UINT64_MAX / PAGE_SIZE) return K_EINVAL;
        uint64_t offset = entry->index * PAGE_SIZE;
        if (offset >= vnode->size) {
            entry->dirty = false;
            continue;
        }
        uint64_t length = vnode->size - offset;
        if (length > PAGE_SIZE) length = PAGE_SIZE;
        uint64_t written = 0U;
        vfs_backend_lock(backend);
        kstatus_t status = backend->write == 0 ? K_EACCES :
            backend->write(backend->context, offset,
                           phys_to_direct(page_to_phys(entry->page)),
                           (size_t)length, &written);
        vfs_backend_unlock(backend);
        if (status != K_OK || written != length) {
            return status == K_OK ? K_EIO : status;
        }
        entry->dirty = false;
    }
    return K_OK;
}

kstatus_t vfs_page_cache_flush(vnode_t *vnode) {
    if (vnode == 0) return K_EINVAL;
    return vfs_page_cache_flush_locked(vnode, 0U, UINT64_MAX);
}

kstatus_t vfs_page_cache_sync(vnode_t *vnode, uint64_t first_page,
                              uint64_t page_count) {
    vfs_backend_node_t *backend;
    kstatus_t status;
    if (vnode == 0 || page_count == 0U) return K_EINVAL;
    vfs_vnode_lock(vnode);
    status = vfs_page_cache_flush_locked(vnode, first_page, page_count);
    vfs_vnode_unlock(vnode);
    if (status != K_OK) return status;
    backend = (vfs_backend_node_t *)vnode->fs_private;
    if (backend == 0 || backend->fsync == 0) return K_OK;
    vfs_backend_lock(backend);
    status = backend->fsync(backend->context);
    vfs_backend_unlock(backend);
    return status;
}

kstatus_t vfs_file_page_get(vnode_t *vnode, uint64_t page_index,
                            struct page **out) {
    if (vnode == 0 || vnode->object.type != KOBJECT_TYPE_VNODE || out == 0) {
        return K_EINVAL;
    }
    vfs_backend_node_t *backend =
        (vfs_backend_node_t *)vnode->fs_private;
    if (backend == 0 || backend->read == 0 ||
        page_index > UINT64_MAX / PAGE_SIZE) return K_EINVAL;
    vfs_vnode_lock(vnode);
    vfs_page_cache_t *cache =
        (vfs_page_cache_t *)vnode->page_cache;
    if (cache == 0) {
        cache = (vfs_page_cache_t *)kzalloc(sizeof(*cache), 0);
        if (cache == 0) {
            vfs_vnode_unlock(vnode);
            return K_ENOMEM;
        }
        cache->head = 0;
        vnode->page_cache = cache;
    }
    for (vfs_cached_page_t *entry = cache->head; entry != 0;
         entry = entry->next) {
        if (entry->index == page_index) {
            *out = entry->page;
            vfs_vnode_unlock(vnode);
            return K_OK;
        }
    }

    page_t *page = page_alloc(0, PAGE_ALLOC_ZERO);
    if (page == 0) {
        vfs_vnode_unlock(vnode);
        return K_ENOMEM;
    }
    page->owner = PAGE_OWNER_FILE;
    page->u.file.mapping = vnode;
    page->u.file.index = page_index;
    uint64_t file_offset = page_index * PAGE_SIZE;
    if (file_offset < backend->size) {
        uint64_t length = backend->size - file_offset;
        if (length > PAGE_SIZE) length = PAGE_SIZE;
        uint64_t bytes = 0U;
        vfs_backend_lock(backend);
        kstatus_t status = backend->read(backend->context, file_offset,
                                         phys_to_direct(page_to_phys(page)),
                                         (size_t)length, &bytes);
        vfs_backend_unlock(backend);
        if (status != K_OK || bytes > length) {
            page_free(page);
            vfs_vnode_unlock(vnode);
            return status == K_OK ? K_EIO : status;
        }
    }
    vfs_cached_page_t *entry =
        (vfs_cached_page_t *)kzalloc(sizeof(*entry), 0);
    if (entry == 0) {
        page_free(page);
        vfs_vnode_unlock(vnode);
        return K_ENOMEM;
    }
    entry->index = page_index;
    entry->page = page;
    entry->dirty = false;
    entry->next = cache->head;
    cache->head = entry;
    *out = page;
    vfs_vnode_unlock(vnode);
    return K_OK;
}

void vfs_file_page_mark_dirty(vnode_t *vnode, uint64_t page_index) {
    if (vnode == 0) return;
    vfs_vnode_lock(vnode);
    vfs_page_cache_t *cache =
        (vfs_page_cache_t *)vnode->page_cache;
    if (cache != 0) {
        for (vfs_cached_page_t *entry = cache->head; entry != 0;
             entry = entry->next) {
            if (entry->index == page_index) {
                entry->dirty = true;
                break;
            }
        }
    }
    vfs_vnode_unlock(vnode);
}

void vfs_page_cache_copy_in(vnode_t *vnode, uint64_t offset,
                            const uint8_t *source, size_t length) {
    if (vnode == 0 || source == 0 || length == 0U) return;
    vfs_vnode_lock(vnode);
    vfs_page_cache_t *cache =
        (vfs_page_cache_t *)vnode->page_cache;
    size_t copied = 0U;
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
            uint8_t *page_memory = (uint8_t *)phys_to_direct(
                page_to_phys(entry->page));
            if (page_memory != 0) {
                for (size_t i = 0U; i < chunk; ++i) {
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
    vfs_vnode_unlock(vnode);
}
