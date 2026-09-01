#include <kernel/handle.h>
#include <kernel/mm.h>
#include <kernel/sched.h>

#define HANDLE_INITIAL_CHUNKS 4U
#define HANDLE_MAX_CHUNKS     65536U
#define HANDLE_INVALID_INDEX  UINT32_MAX

static atomic_uintptr_t g_handle_waiting_lock;

static void handle_lock(spinlock_t *lock) {
    /*
     * The table lock is also held while a new chunk or backing array is
     * allocated.  Keep its owner on this CPU until the publication is done;
     * otherwise a local timer can switch away while another CPU spins here.
    */
    sched_preempt_disable();
    atomic_store_explicit(&g_handle_waiting_lock, (uintptr_t)lock,
                          memory_order_release);
    while (atomic_exchange_explicit(&lock->state, 1U,
                                     memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
    atomic_store_explicit(&g_handle_waiting_lock, 0U, memory_order_release);
}

static void handle_unlock(spinlock_t *lock) {
    atomic_store_explicit(&lock->state, 0U, memory_order_release);
    sched_preempt_enable();
}

static void chunk_lock(handle_chunk_t *chunk) {
    sched_preempt_disable();
    atomic_store_explicit(&g_handle_waiting_lock, (uintptr_t)&chunk->lock,
                          memory_order_release);
    while (atomic_exchange_explicit(&chunk->lock.state, 1U,
                                     memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
    atomic_store_explicit(&g_handle_waiting_lock, 0U, memory_order_release);
}

static void chunk_unlock(handle_chunk_t *chunk) {
    atomic_store_explicit(&chunk->lock.state, 0U, memory_order_release);
    sched_preempt_enable();
}

static handle_t make_handle(uint32_t index, uint32_t generation) {
    return ((uint64_t)generation << HANDLE_INDEX_BITS) | index;
}

static uint32_t handle_index(handle_t handle) {
    return (uint32_t)handle;
}

static uint32_t handle_generation(handle_t handle) {
    return (uint32_t)(handle >> HANDLE_INDEX_BITS);
}

/* Handle storage does not know concrete object Owners. */
static void handle_close_object(void *object) {
    if (object == 0) return;
    object_header_t *header = (object_header_t *)object;
    if (header->ops != 0 && header->ops->handle_close != 0) {
        header->ops->handle_close(object);
    }
}

static handle_chunk_t *chunk_allocate(void) {
    handle_chunk_t *chunk = (handle_chunk_t *)kzalloc(sizeof(handle_chunk_t), 0);
    if (chunk == 0) return 0;
    atomic_init(&chunk->lock.state, 0U);
    for (uint32_t i = 0; i < HANDLE_CHUNK_ENTRIES; ++i) {
        chunk->entries[i].generation = 1U;
        chunk->entries[i].next_free = HANDLE_INVALID_INDEX;
    }
    return chunk;
}

static kstatus_t grow_table_locked(handle_table_t *table) {
    if (table->chunk_capacity >= HANDLE_MAX_CHUNKS) return K_ENOMEM;
    uint32_t new_capacity = table->chunk_capacity == 0 ? HANDLE_INITIAL_CHUNKS :
                            table->chunk_capacity * 2U;
    if (new_capacity > HANDLE_MAX_CHUNKS || new_capacity < table->chunk_capacity) {
        new_capacity = HANDLE_MAX_CHUNKS;
    }
    handle_chunk_t **chunks = (handle_chunk_t **)kzalloc(
        (size_t)new_capacity * sizeof(handle_chunk_t *), 0);
    if (chunks == 0) return K_ENOMEM;
    for (uint32_t i = 0; i < table->chunk_capacity; ++i) chunks[i] = table->chunks[i];
    handle_chunk_t **old = table->chunks;
    table->chunks = chunks;
    table->chunk_capacity = new_capacity;
    kfree(old);
    return K_OK;
}

kstatus_t handle_table_init(handle_table_t *table) {
    if (table == 0) return K_EINVAL;
    table->chunks = 0;
    table->chunk_count = 0;
    table->chunk_capacity = 0;
    table->free_hint = 0;
    table->reserved = 0;
    atomic_init(&table->grow_lock.state, 0U);
    handle_lock(&table->grow_lock);
    kstatus_t status = grow_table_locked(table);
    handle_unlock(&table->grow_lock);
    return status;
}

static kstatus_t handle_table_clone_internal(handle_table_t *source,
                                             handle_table_t *destination,
                                             bool exclude_type,
                                             object_type_id_t excluded_type) {
    if (source == 0 || destination == 0 || source == destination) return K_EINVAL;

    destination->chunks = 0;
    destination->chunk_count = 0;
    destination->chunk_capacity = 0;
    destination->free_hint = 0;
    destination->reserved = 0;
    atomic_init(&destination->grow_lock.state, 0U);

    /* Keep indexes and generations identical: user-space descriptor tables
     * contain the original kernel handle values after a COW fork. */
    handle_lock(&source->grow_lock);
    uint32_t capacity = source->chunk_capacity;
    uint32_t chunk_count = source->chunk_count;
    if (capacity == 0U || capacity > HANDLE_MAX_CHUNKS ||
        chunk_count > capacity) {
        handle_unlock(&source->grow_lock);
        return K_EINVAL;
    }

    destination->chunks = (handle_chunk_t **)kzalloc(
        (size_t)capacity * sizeof(handle_chunk_t *), 0);
    if (destination->chunks == 0) {
        handle_unlock(&source->grow_lock);
        return K_ENOMEM;
    }
    destination->chunk_capacity = capacity;
    destination->chunk_count = chunk_count;
    destination->free_hint = source->free_hint;
    destination->reserved = source->reserved;

    kstatus_t status = K_OK;
    for (uint32_t chunk_index = 0U; chunk_index < chunk_count; ++chunk_index) {
        handle_chunk_t *source_chunk = source->chunks[chunk_index];
        if (source_chunk == 0) continue;
        handle_chunk_t *destination_chunk = chunk_allocate();
        if (destination_chunk == 0) {
            status = K_ENOMEM;
            break;
        }
        destination->chunks[chunk_index] = destination_chunk;
        chunk_lock(source_chunk);
        for (uint32_t entry_index = 0U;
             entry_index < HANDLE_CHUNK_ENTRIES; ++entry_index) {
            handle_entry_t *source_entry = &source_chunk->entries[entry_index];
            handle_entry_t *destination_entry =
                &destination_chunk->entries[entry_index];
            *destination_entry = *source_entry;
            if (source_entry->object == 0) continue;
            if (exclude_type &&
                ((object_header_t *)source_entry->object)->type == excluded_type) {
                destination_entry->object = 0;
                destination_entry->rights = 0;
                destination_entry->flags = 0;
                continue;
            }
            object_get(source_entry->object);
        }
        chunk_unlock(source_chunk);
    }
    handle_unlock(&source->grow_lock);

    if (status != K_OK) {
        handle_table_destroy(destination);
        return status;
    }
    return K_OK;
}

kstatus_t handle_table_clone(handle_table_t *source, handle_table_t *destination) {
    return handle_table_clone_internal(source, destination, false, 0U);
}

kstatus_t handle_table_clone_without_type(handle_table_t *source,
                                          handle_table_t *destination,
                                          object_type_id_t excluded_type) {
    return handle_table_clone_internal(source, destination, true, excluded_type);
}

kstatus_t handle_create_with_flags(handle_table_t *table, void *object,
                                   uint32_t rights, uint32_t flags,
                                   handle_t *out) {
    if (table == 0 || object == 0 || out == 0 || table->chunks == 0 ||
        (flags & ~HANDLE_FLAG_MASK) != 0U) return K_EINVAL;
    handle_lock(&table->grow_lock);
    for (;;) {
        uint64_t capacity = (uint64_t)table->chunk_capacity * HANDLE_CHUNK_ENTRIES;
        for (uint64_t raw_index = table->free_hint; raw_index < capacity; ++raw_index) {
            uint32_t index = (uint32_t)raw_index;
            uint32_t chunk_index = index >> HANDLE_CHUNK_SHIFT;
            uint32_t entry_index = index & (HANDLE_CHUNK_ENTRIES - 1U);
            handle_chunk_t *chunk = table->chunks[chunk_index];
            if (chunk == 0) {
                chunk = chunk_allocate();
                if (chunk == 0) {
                    handle_unlock(&table->grow_lock);
                    return K_ENOMEM;
                }
                table->chunks[chunk_index] = chunk;
                if (chunk_index + 1U > table->chunk_count) table->chunk_count = chunk_index + 1U;
            }
            chunk_lock(chunk);
            handle_entry_t *entry = &chunk->entries[entry_index];
            if (entry->object == 0) {
                object_get(object);
                entry->object = object;
                entry->rights = rights;
                entry->flags = flags;
                entry->next_free = HANDLE_INVALID_INDEX;
                table->free_hint = index + 1U;
                *out = make_handle(index, entry->generation);
                chunk_unlock(chunk);
                handle_unlock(&table->grow_lock);
                return K_OK;
            }
            chunk_unlock(chunk);
        }
        kstatus_t status = grow_table_locked(table);
        if (status != K_OK) {
            handle_unlock(&table->grow_lock);
            return status;
        }
    }
}

kstatus_t handle_create(handle_table_t *table, void *object, uint32_t rights,
                        handle_t *out) {
    return handle_create_with_flags(table, object, rights, 0U, out);
}

/* Take a temporary object reference while reading a source entry. */
static kstatus_t handle_reference(handle_table_t *table, handle_t handle,
                                  uint32_t *rights, void **object) {
    uint32_t index;
    uint32_t chunk_index;
    uint32_t entry_index;
    handle_chunk_t *chunk;
    handle_entry_t *entry;
    kstatus_t status = K_OK;

    if (table == 0 || rights == 0 || object == 0 || handle == 0) {
        return K_EINVAL;
    }
    index = handle_index(handle);
    chunk_index = index >> HANDLE_CHUNK_SHIFT;
    entry_index = index & (HANDLE_CHUNK_ENTRIES - 1U);
    handle_lock(&table->grow_lock);
    if (chunk_index >= table->chunk_capacity ||
        table->chunks[chunk_index] == 0) {
        handle_unlock(&table->grow_lock);
        return K_ENOENT;
    }
    chunk = table->chunks[chunk_index];
    chunk_lock(chunk);
    entry = &chunk->entries[entry_index];
    if (entry->object == 0 || entry->generation != handle_generation(handle) ||
        !object_try_get(entry->object)) {
        status = K_ENOENT;
    } else {
        *rights = entry->rights;
        *object = entry->object;
    }
    chunk_unlock(chunk);
    handle_unlock(&table->grow_lock);
    return status;
}

kstatus_t handle_duplicate(handle_table_t *table, handle_t source,
                           uint32_t flags, handle_t *out) {
    void *object = 0;
    uint32_t rights = 0U;
    kstatus_t status;

    if ((flags & ~HANDLE_FLAG_MASK) != 0U || out == 0) return K_EINVAL;
    status = handle_reference(table, source, &rights, &object);
    if (status != K_OK) return status;
    status = handle_create_with_flags(table, object, rights, flags, out);
    object_put(object);
    return status;
}

kstatus_t handle_lookup(handle_table_t *table, handle_t handle, uint32_t rights,
                        void **out_object) {
    if (table == 0 || out_object == 0 || handle == 0) return K_EINVAL;
    uint32_t index = handle_index(handle);
    uint32_t chunk_index = index >> HANDLE_CHUNK_SHIFT;
    uint32_t entry_index = index & (HANDLE_CHUNK_ENTRIES - 1U);
    handle_lock(&table->grow_lock);
    if (chunk_index >= table->chunk_capacity || table->chunks[chunk_index] == 0) {
        handle_unlock(&table->grow_lock);
        return K_ENOENT;
    }
    handle_chunk_t *chunk = table->chunks[chunk_index];
    chunk_lock(chunk);
    handle_entry_t *entry = &chunk->entries[entry_index];
    kstatus_t status = K_OK;
    if (entry->object == 0 || entry->generation != handle_generation(handle)) {
        status = K_ENOENT;
    } else if ((entry->rights & rights) != rights) {
        status = K_EACCES;
    } else if (!object_try_get(entry->object)) {
        status = K_ENOENT;
    } else {
        *out_object = entry->object;
    }
    chunk_unlock(chunk);
    handle_unlock(&table->grow_lock);
    return status;
}

kstatus_t handle_get_flags(handle_table_t *table, handle_t handle,
                           uint32_t *flags) {
    uint32_t index;
    uint32_t chunk_index;
    uint32_t entry_index;
    handle_chunk_t *chunk;
    handle_entry_t *entry;
    kstatus_t status = K_OK;

    if (table == 0 || flags == 0 || handle == 0) return K_EINVAL;
    index = handle_index(handle);
    chunk_index = index >> HANDLE_CHUNK_SHIFT;
    entry_index = index & (HANDLE_CHUNK_ENTRIES - 1U);
    handle_lock(&table->grow_lock);
    if (chunk_index >= table->chunk_capacity ||
        table->chunks[chunk_index] == 0) {
        handle_unlock(&table->grow_lock);
        return K_ENOENT;
    }
    chunk = table->chunks[chunk_index];
    chunk_lock(chunk);
    entry = &chunk->entries[entry_index];
    if (entry->object == 0 || entry->generation != handle_generation(handle)) {
        status = K_ENOENT;
    } else {
        *flags = entry->flags;
    }
    chunk_unlock(chunk);
    handle_unlock(&table->grow_lock);
    return status;
}

kstatus_t handle_set_flags(handle_table_t *table, handle_t handle,
                           uint32_t flags) {
    uint32_t index;
    uint32_t chunk_index;
    uint32_t entry_index;
    handle_chunk_t *chunk;
    handle_entry_t *entry;
    kstatus_t status = K_OK;

    if (table == 0 || handle == 0 || (flags & ~HANDLE_FLAG_MASK) != 0U) {
        return K_EINVAL;
    }
    index = handle_index(handle);
    chunk_index = index >> HANDLE_CHUNK_SHIFT;
    entry_index = index & (HANDLE_CHUNK_ENTRIES - 1U);
    handle_lock(&table->grow_lock);
    if (chunk_index >= table->chunk_capacity ||
        table->chunks[chunk_index] == 0) {
        handle_unlock(&table->grow_lock);
        return K_ENOENT;
    }
    chunk = table->chunks[chunk_index];
    chunk_lock(chunk);
    entry = &chunk->entries[entry_index];
    if (entry->object == 0 || entry->generation != handle_generation(handle)) {
        status = K_ENOENT;
    } else {
        entry->flags = flags;
    }
    chunk_unlock(chunk);
    handle_unlock(&table->grow_lock);
    return status;
}

kstatus_t handle_close(handle_table_t *table, handle_t handle) {
    if (table == 0 || handle == 0) return K_EINVAL;
    uint32_t index = handle_index(handle);
    uint32_t chunk_index = index >> HANDLE_CHUNK_SHIFT;
    uint32_t entry_index = index & (HANDLE_CHUNK_ENTRIES - 1U);
    handle_lock(&table->grow_lock);
    if (chunk_index >= table->chunk_capacity || table->chunks[chunk_index] == 0) {
        handle_unlock(&table->grow_lock);
        return K_ENOENT;
    }
    handle_chunk_t *chunk = table->chunks[chunk_index];
    chunk_lock(chunk);
    handle_entry_t *entry = &chunk->entries[entry_index];
    if (entry->object == 0 || entry->generation != handle_generation(handle)) {
        chunk_unlock(chunk);
        handle_unlock(&table->grow_lock);
        return K_ENOENT;
    }
    void *object = entry->object;
    entry->object = 0;
    entry->rights = 0;
    entry->flags = 0;
    ++entry->generation;
    if (entry->generation == 0) entry->generation = 1U;
    if (index < table->free_hint) table->free_hint = index;
    chunk_unlock(chunk);
    handle_unlock(&table->grow_lock);
    handle_close_object(object);
    object_put(object);
    return K_OK;
}

kstatus_t handle_table_close_on_exec(handle_table_t *table) {
    if (table == 0) return K_EINVAL;

    /* Remove one matching entry at a time so object callbacks run outside the
     * table locks and a generation change cannot invalidate a later scan. */
    for (;;) {
        handle_t candidate = 0;
        handle_lock(&table->grow_lock);
        for (uint32_t chunk_index = 0U;
             chunk_index < table->chunk_count && candidate == 0;
             ++chunk_index) {
            handle_chunk_t *chunk = table->chunks[chunk_index];
            if (chunk == 0) continue;
            chunk_lock(chunk);
            for (uint32_t entry_index = 0U;
                 entry_index < HANDLE_CHUNK_ENTRIES; ++entry_index) {
                handle_entry_t *entry = &chunk->entries[entry_index];
                if (entry->object != 0 &&
                    (entry->flags & HANDLE_FLAG_CLOEXEC) != 0U) {
                    uint32_t index = (chunk_index << HANDLE_CHUNK_SHIFT) |
                                     entry_index;
                    candidate = make_handle(index, entry->generation);
                    break;
                }
            }
            chunk_unlock(chunk);
        }
        handle_unlock(&table->grow_lock);
        if (candidate == 0) return K_OK;
        kstatus_t status = handle_close(table, candidate);
        if (status != K_OK && status != K_ENOENT) return status;
    }
}

void handle_table_destroy(handle_table_t *table) {
    if (table == 0) return;
    handle_lock(&table->grow_lock);
    for (uint32_t chunk_index = 0; chunk_index < table->chunk_capacity; ++chunk_index) {
        handle_chunk_t *chunk = table->chunks[chunk_index];
        if (chunk == 0) continue;
        chunk_lock(chunk);
        for (uint32_t i = 0; i < HANDLE_CHUNK_ENTRIES; ++i) {
            void *object = chunk->entries[i].object;
            chunk->entries[i].object = 0;
            if (object != 0) {
                handle_close_object(object);
                object_put(object);
            }
        }
        chunk_unlock(chunk);
        kfree(chunk);
        table->chunks[chunk_index] = 0;
    }
    handle_chunk_t **chunks = table->chunks;
    table->chunks = 0;
    table->chunk_count = 0;
    table->chunk_capacity = 0;
    table->free_hint = 0;
    handle_unlock(&table->grow_lock);
    kfree(chunks);
}

uintptr_t handle_debug_waiting_lock(void) {
    return atomic_load_explicit(&g_handle_waiting_lock, memory_order_acquire);
}
