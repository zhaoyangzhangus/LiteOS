#include <kernel/handle.h>
#include <kernel/mm.h>
#include <kernel/completion_port.h>
#include <kernel/message_port.h>
#include <kernel/timer.h>
#include <kernel/socket.h>
#include <kernel/window_server.h>

#define HANDLE_INITIAL_CHUNKS 4U
#define HANDLE_MAX_CHUNKS     65536U
#define HANDLE_INVALID_INDEX  UINT32_MAX

static void handle_lock(spinlock_t *lock) {
    while (atomic_exchange_explicit(&lock->state, 1U,
                                     memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
}

static void handle_unlock(spinlock_t *lock) {
    atomic_store_explicit(&lock->state, 0U, memory_order_release);
}

static void chunk_lock(handle_chunk_t *chunk) {
    while (atomic_exchange_explicit(&chunk->lock.state, 1U,
                                     memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
}

static void chunk_unlock(handle_chunk_t *chunk) {
    atomic_store_explicit(&chunk->lock.state, 0U, memory_order_release);
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

kstatus_t handle_create(handle_table_t *table, void *object, uint32_t rights, handle_t *out) {
    if (table == 0 || object == 0 || out == 0 || table->chunks == 0) return K_EINVAL;
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
                entry->flags = 0;
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
    object_header_t *header = (object_header_t *)object;
    if (header->type == KOBJECT_TYPE_SOCKET) (void)socket_close((socket_t *)object);
    else if (header->type == KOBJECT_TYPE_COMPLETION_PORT) {
        (void)completion_port_close((completion_port_t *)object);
    } else if (header->type == KOBJECT_TYPE_MESSAGE_PORT) {
        (void)message_port_close((message_port_t *)object);
    } else if (header->type == KOBJECT_TYPE_TIMER) {
        (void)timer_cancel((timer_object_t *)object);
    } else if (header->type == KOBJECT_TYPE_WINDOW) {
        window_server_handle_closed((window_server_window_t *)object);
    }
    object_put(object);
    return K_OK;
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
                object_header_t *header = (object_header_t *)object;
                if (header->type == KOBJECT_TYPE_SOCKET) {
                    (void)socket_close((socket_t *)object);
                } else if (header->type == KOBJECT_TYPE_COMPLETION_PORT) {
                    (void)completion_port_close((completion_port_t *)object);
                } else if (header->type == KOBJECT_TYPE_MESSAGE_PORT) {
                    (void)message_port_close((message_port_t *)object);
                } else if (header->type == KOBJECT_TYPE_TIMER) {
                    (void)timer_cancel((timer_object_t *)object);
                } else if (header->type == KOBJECT_TYPE_WINDOW) {
                    window_server_handle_closed((window_server_window_t *)object);
                }
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
