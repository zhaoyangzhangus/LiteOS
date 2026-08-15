#pragma once
#include "base.h"
#include "object.h"
#include "spinlock.h"

typedef uint64_t handle_t;

#define HANDLE_INDEX_BITS 32u
#define HANDLE_GENERATION_BITS 32u
#define HANDLE_CHUNK_SHIFT 8u
#define HANDLE_CHUNK_ENTRIES (1u << HANDLE_CHUNK_SHIFT)

typedef struct handle_entry {
    void *object;
    uint32_t generation;
    uint32_t rights;
    uint32_t flags;
    uint32_t next_free;
} handle_entry_t;

typedef struct handle_chunk {
    rwlock_t lock;
    handle_entry_t entries[HANDLE_CHUNK_ENTRIES];
} handle_chunk_t;

typedef struct handle_table {
    spinlock_t grow_lock;
    handle_chunk_t **chunks;
    uint32_t chunk_count;
    uint32_t chunk_capacity;
    uint32_t free_hint;
    uint32_t reserved;
} handle_table_t;

kstatus_t handle_table_init(handle_table_t *table);
kstatus_t handle_create(handle_table_t *table, void *object, uint32_t rights, handle_t *out);
kstatus_t handle_lookup(handle_table_t *table, handle_t h, uint32_t rights, void **out_object);
kstatus_t handle_close(handle_table_t *table, handle_t h);
