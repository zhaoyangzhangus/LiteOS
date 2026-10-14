#include "internal.h"
#include <kernel/vm.h>
#include <kernel/string.h>

/* REFACTOR_FS_VFS_PAGE_CACHE_OWNER: file-page cache lifetime and I/O. */

static uint32_t vfs_page_cache_bucket(uint64_t page_index, uint32_t count) {
    page_index ^= page_index >> 32;
    page_index ^= page_index >> 16;
    page_index ^= page_index >> 8;
    return (uint32_t)(page_index & (count - 1U));
}

static uint32_t vfs_page_cache_bucket_count(vfs_page_cache_t *cache) {
    return cache->bucket_count != 0U ? cache->bucket_count : VFS_PAGE_CACHE_BUCKETS;
}

/* Shared by writeback and resize; a narrow dense range benefits from hash
 * probes, while wide or sparse ranges need one resident-list walk. */
static inline bool vfs_page_cache_use_range_index(vfs_page_cache_t *cache,
                                                 uint64_t page_count) {
    return page_count <= 16U ||
        (page_count <= vfs_page_cache_bucket_count(cache) &&
         page_count <= cache->entry_count / 4U);
}

static vfs_cached_page_t **vfs_page_cache_buckets(vfs_page_cache_t *cache) {
    return cache->expanded_buckets != 0 ? cache->expanded_buckets : cache->buckets;
}

/* Caller holds the vnode lock. Growth is optional: allocation failure must
 * not discard cached pages or fail a demand read. Retry at most every 256
 * inserted pages, and cap the per-file index at 128 KiB. */
static void vfs_page_cache_grow(vfs_page_cache_t *cache) {
    uint32_t old_count = vfs_page_cache_bucket_count(cache);
    if (old_count >= 16384U || cache->entry_count < (uint64_t)old_count * 4U ||
        cache->entry_count < cache->growth_retry_at) return;
    uint32_t new_count = old_count * 2U;
    vfs_cached_page_t **replacement = kzalloc(
        (size_t)new_count * sizeof(*replacement), 0);
    if (replacement == 0) {
        cache->growth_retry_at = cache->entry_count + VFS_PAGE_CACHE_BUCKETS;
        return;
    }
    for (vfs_cached_page_t *entry = cache->head; entry != 0; entry = entry->next) {
        uint32_t bucket = vfs_page_cache_bucket(entry->index, new_count);
        entry->hash_next = replacement[bucket];
        replacement[bucket] = entry;
    }
    vfs_cached_page_t **previous = cache->expanded_buckets;
    cache->expanded_buckets = replacement;
    cache->bucket_count = new_count;
    cache->growth_retry_at = 0U;
    kfree(previous);
}

static void vfs_page_cache_insert(vfs_page_cache_t *cache, vfs_cached_page_t *entry) {
    vfs_cached_page_t **buckets = vfs_page_cache_buckets(cache);
    uint32_t bucket = vfs_page_cache_bucket(entry->index, vfs_page_cache_bucket_count(cache));
    entry->hash_next = buckets[bucket];
    buckets[bucket] = entry;
    entry->next = cache->head;
    cache->head = entry;
    ++cache->entry_count;
}

static vfs_cached_page_t *vfs_page_cache_lookup(vfs_page_cache_t *cache,
                                                 uint64_t page_index) {
    if (cache == 0) return 0;
    for (vfs_cached_page_t *entry =
             vfs_page_cache_buckets(cache)[vfs_page_cache_bucket(
                 page_index, vfs_page_cache_bucket_count(cache))];
         entry != 0; entry = entry->hash_next) {
        if (entry->index == page_index) return entry;
    }
    return 0;
}

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
    kfree(cache->expanded_buckets);
    kfree(cache);
    vnode->page_cache = 0;
}

/* Caller holds vnode->lock.  A bounded page range keeps VM_SYNC from flushing
 * unrelated dirty mappings that happen to share the same vnode. */
static kstatus_t vfs_page_cache_flush_entry(vnode_t *vnode,
                                            vfs_backend_node_t *backend,
                                            vfs_cached_page_t *entry) {
    if (entry == 0 || !entry->dirty) return K_OK;
    if (entry->index > UINT64_MAX / PAGE_SIZE) return K_EINVAL;
    uint64_t offset = entry->index * PAGE_SIZE;
    if (offset >= vnode->size) {
        entry->dirty = false;
        return K_OK;
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
    return K_OK;
}

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
    /* Keep narrow ranges on the index even in large files: a fixed 16-page
     * cutoff made a 17-page msync walk every resident page. Bound probing by
     * both bucket count and resident pages; wide/full ranges use one walk. */
    if (vfs_page_cache_use_range_index(cache, page_count)) {
        for (uint64_t i = 0U; i < page_count; ++i) {
            kstatus_t status = vfs_page_cache_flush_entry(vnode, backend,
                vfs_page_cache_lookup(cache, first_page + i));
            if (status != K_OK) return status;
        }
        return K_OK;
    }
    for (vfs_cached_page_t *entry = cache->head; entry != 0;
         entry = entry->next) {
        if (!entry->dirty || entry->index < first_page ||
            entry->index > last_page) continue;
        kstatus_t status = vfs_page_cache_flush_entry(vnode, backend, entry);
        if (status != K_OK) return status;
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
    vfs_cached_page_t *cached = vfs_page_cache_lookup(cache, page_index);
    if (cached != 0) {
        *out = cached->page;
        vfs_vnode_unlock(vnode);
        return K_OK;
    }

    uint64_t file_offset = page_index * PAGE_SIZE;
    uint64_t next_index = page_index == UINT64_MAX ? UINT64_MAX :
                          page_index + 1U;
    BOOLEAN prefetch = next_index != UINT64_MAX &&
                       file_offset <= backend->size &&
                       backend->size - file_offset > PAGE_SIZE &&
                       vfs_page_cache_lookup(cache, next_index) == 0;
    vfs_cached_page_t *entry =
        (vfs_cached_page_t *)kzalloc(sizeof(*entry), 0);
    vfs_cached_page_t *next_entry = 0;
    page_t *page;
    uint64_t read_length;
    uint64_t bytes = 0U;
    if (entry == 0) {
        vfs_vnode_unlock(vnode);
        return K_ENOMEM;
    }
    if (prefetch) {
        next_entry = (vfs_cached_page_t *)kzalloc(sizeof(*next_entry), 0);
        if (next_entry == 0) prefetch = 0;
    }
    page = page_alloc(prefetch ? 1U : 0U, 0);
    if (page == 0 && prefetch) {
        /* Read-ahead is optional; fragmentation must not fail the demand page. */
        kfree(next_entry);
        next_entry = 0;
        prefetch = 0;
        page = page_alloc(0U, 0);
    }
    if (page == 0) {
        kfree(next_entry);
        kfree(entry);
        vfs_vnode_unlock(vnode);
        return K_ENOMEM;
    }
    page->owner = PAGE_OWNER_FILE;
    page->u.file.mapping = vnode;
    page->u.file.index = page_index;
    paddr_t physical = page_to_phys(page);
    uint8_t *memory = phys_to_direct(physical);
    read_length = backend->size > file_offset ? backend->size - file_offset : 0U;
    if (read_length > (prefetch ? 2U * PAGE_SIZE : PAGE_SIZE)) {
        read_length = prefetch ? 2U * PAGE_SIZE : PAGE_SIZE;
    }
    if (read_length != 0U) {
        vfs_backend_lock(backend);
        kstatus_t status = backend->read(backend->context, file_offset,
                                         memory,
                                         (size_t)read_length, &bytes);
        vfs_backend_unlock(backend);
        if (status != K_OK || bytes != read_length) {
            page_free(page);
            kfree(next_entry);
            kfree(entry);
            vfs_vnode_unlock(vnode);
            return status == K_OK ? K_EIO : status;
        }
    }
    /* Publish no stale allocation bytes beyond EOF. Fully read pages need
     * no preliminary clear; failed/short reads are freed above. */
    size_t capacity = prefetch ? 2U * PAGE_SIZE : PAGE_SIZE;
    if (read_length < capacity) memset(memory + read_length, 0, capacity - read_length);
    if (prefetch && bytes <= PAGE_SIZE) {
        kfree(next_entry);
        next_entry = 0;
        prefetch = 0;
    }
    entry->index = page_index;
    entry->page = page;
    entry->dirty = false;
    vfs_page_cache_insert(cache, entry);
    if (prefetch) {
        page_t *next_page = phys_to_page(paddr_make(
            physical.value + PAGE_SIZE));
        if (next_page == 0 ||
            (next_page->flags & PAGE_COMPOUND_TAIL) == 0U) {
            uint32_t bucket = vfs_page_cache_bucket(page_index,
                vfs_page_cache_bucket_count(cache));
            vfs_page_cache_buckets(cache)[bucket] = entry->hash_next;
            cache->head = entry->next;
            --cache->entry_count;
            page_free(page);
            kfree(next_entry);
            kfree(entry);
            vfs_vnode_unlock(vnode);
            return K_EIO;
        }
        next_page->owner = PAGE_OWNER_FILE;
        next_page->u.file.mapping = vnode;
        next_page->u.file.index = next_index;
        next_entry->index = next_index;
        next_entry->page = next_page;
        next_entry->dirty = false;
        vfs_page_cache_insert(cache, next_entry);
    }
    vfs_page_cache_grow(cache);
    *out = page;
    vfs_vnode_unlock(vnode);
    return K_OK;
}

kstatus_t vfs_read_cached_kernel(file_t *file, void *buffer, size_t length,
                                 uint64_t *bytes) {
    if (file == 0 || file->object.type != KOBJECT_TYPE_FILE ||
        file->vnode == 0 || buffer == 0 || bytes == 0 ||
        (file->rights & VFS_OPEN_READ) == 0U) return K_EINVAL;
    *bytes = 0U;
    size_t copied = 0U;
    kstatus_t result = K_OK;
    vnode_t *vnode = file->vnode;
    while (copied < length) {
        vfs_vnode_lock(vnode);
        uint64_t position = file->position;
        bool eof = position >= vnode->size;
        vfs_vnode_unlock(vnode);
        if (eof) break;
        page_t *page = 0;
        kstatus_t status = vfs_file_page_get(vnode, position / PAGE_SIZE, &page);
        if (status != K_OK) {
            result = status;
            break;
        }
        if (page == 0) {
            result = K_EIO;
            break;
        }
        uint8_t *source = phys_to_direct(page_to_phys(page));
        if (source == 0) {
            result = K_EIO;
            break;
        }
        size_t offset = position & (PAGE_SIZE - 1ULL);
        size_t chunk = PAGE_SIZE - offset;
        if (chunk > length - copied) chunk = length - copied;
        /* File ownership keeps the borrowed cache page alive. Serialize
         * copying with cache updates, and recheck EOF after backing I/O. */
        vfs_vnode_lock(vnode);
        if (position >= vnode->size) {
            vfs_vnode_unlock(vnode);
            break;
        }
        if (chunk > vnode->size - position) chunk = vnode->size - position;
        __builtin_memcpy((uint8_t *)buffer + copied, source + offset, chunk);
        file->position = position + chunk;
        vfs_vnode_unlock(vnode);
        copied += chunk;
    }
    *bytes = copied;
    return result;
}

static void vfs_page_cache_zero_range(vfs_cached_page_t *entry,
                                      uint64_t start, uint64_t end,
                                      uint64_t size) {
    if (entry == 0) return;
    uint64_t base = entry->index * PAGE_SIZE;
    if (base >= end) return;
    uint64_t low = start > base ? start - base : 0U;
    uint64_t high = end - base;
    if (low >= PAGE_SIZE) return;
    if (high > PAGE_SIZE) high = PAGE_SIZE;
    uint8_t *memory = phys_to_direct(page_to_phys(entry->page));
    if (memory != 0) memset(memory + low, 0, (size_t)(high - low));
    if (base >= size) entry->dirty = false;
}

void vfs_page_cache_resize(vnode_t *vnode, uint64_t size) {
    if (vnode == 0) return;
    vfs_vnode_lock(vnode);
    uint64_t old_size = vnode->size;
    if (size == old_size) {
        vfs_vnode_unlock(vnode);
        return;
    }
    uint64_t start = size < old_size ? size : old_size;
    uint64_t end = size < old_size ? UINT64_MAX : size;
    vfs_page_cache_t *cache = vnode->page_cache;
    if (cache != 0 && size > old_size) {
        uint64_t first = start / PAGE_SIZE;
        uint64_t count = (size - 1U) / PAGE_SIZE - first + 1U;
        /* Small growth must not sweep an entire large cached executable.
         * Bound hash probing like range writeback; retain one walk for
         * wide/sparse growth. Shrink also clears pages beyond old EOF. */
        if (vfs_page_cache_use_range_index(cache, count)) {
            for (uint64_t i = 0; i < count; ++i) {
                vfs_page_cache_zero_range(vfs_page_cache_lookup(cache, first + i),
                                           start, end, size);
            }
            vnode->size = size;
            vfs_vnode_unlock(vnode);
            return;
        }
    }
    for (vfs_cached_page_t *entry = cache != 0 ? cache->head : 0;
         entry != 0; entry = entry->next) {
        vfs_page_cache_zero_range(entry, start, end, size);
    }
    vnode->size = size;
    vfs_vnode_unlock(vnode);
}

void vfs_file_page_mark_dirty(vnode_t *vnode, uint64_t page_index) {
    if (vnode == 0) return;
    vfs_vnode_lock(vnode);
    vfs_page_cache_t *cache =
        (vfs_page_cache_t *)vnode->page_cache;
    if (cache != 0) {
        vfs_cached_page_t *entry = vfs_page_cache_lookup(cache, page_index);
        if (entry != 0) entry->dirty = true;
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
        vfs_cached_page_t *entry = vfs_page_cache_lookup(cache, index);
        if (entry != 0) {
            uint8_t *page_memory = (uint8_t *)phys_to_direct(
                page_to_phys(entry->page));
            if (page_memory != 0) {
                memmove(page_memory + in_page, source + copied, chunk);
                /* The backend has persisted only this write's bytes. Other
                 * mmap modifications in a partially covered page still need
                 * writeback; only a complete replacement consumes them. */
                if (chunk == PAGE_SIZE) entry->dirty = false;
            }
        }
        copied += chunk;
    }
    if (offset <= UINT64_MAX - length && offset + length > vnode->size) {
        vnode->size = offset + length;
    }
    vfs_vnode_unlock(vnode);
}
