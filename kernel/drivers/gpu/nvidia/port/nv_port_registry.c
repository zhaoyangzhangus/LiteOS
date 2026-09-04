#include "nv_port.h"

static size_t nv_port_registry_string_length(const char *value,
                                             size_t capacity) {
    if (value == 0) return capacity;
    for (size_t index = 0U; index < capacity; ++index) {
        if (value[index] == '\0') return index;
    }
    return capacity;
}

static bool nv_port_registry_key_valid(const char *key) {
    size_t length = nv_port_registry_string_length(key,
                                                   NV_PORT_REGISTRY_KEY_MAX);
    return key != 0 && length != 0U && length < NV_PORT_REGISTRY_KEY_MAX;
}

static bool nv_port_registry_key_equal(const char *left, const char *right) {
    if (left == 0 || right == 0) return false;
    for (size_t index = 0U; index < NV_PORT_REGISTRY_KEY_MAX; ++index) {
        if (left[index] != right[index]) return false;
        if (left[index] == '\0') return true;
    }
    return false;
}

static void nv_port_registry_copy(char *destination, const char *source,
                                  size_t capacity) {
    for (size_t index = 0U; index < capacity; ++index) {
        destination[index] = source[index];
        if (source[index] == '\0') return;
    }
    destination[capacity - 1U] = '\0';
}

static int32_t nv_port_registry_find_locked(nv_port_registry_t *registry,
                                            const char *key) {
    for (uint32_t index = 0U; index < NV_PORT_REGISTRY_MAX_ENTRIES; ++index) {
        if (registry->entries[index].present &&
            nv_port_registry_key_equal(registry->entries[index].key, key)) {
            return (int32_t)index;
        }
    }
    return -1;
}

static int32_t nv_port_registry_free_locked(nv_port_registry_t *registry) {
    for (uint32_t index = 0U; index < NV_PORT_REGISTRY_MAX_ENTRIES; ++index) {
        if (!registry->entries[index].present) return (int32_t)index;
    }
    return -1;
}

void nv_port_registry_init(nv_port_registry_t *registry) {
    if (registry == 0) return;
    nv_port_spin_init(&registry->lock);
    for (uint32_t index = 0U; index < NV_PORT_REGISTRY_MAX_ENTRIES; ++index) {
        registry->entries[index] = (nv_port_registry_entry_t){0};
    }
    registry->initialized = true;
}

static kstatus_t nv_port_registry_set(nv_port_registry_t *registry,
                                       const char *key, uint32_t type,
                                       uint32_t u32, uint64_t u64,
                                       const char *string) {
    if (registry == 0 || !registry->initialized ||
        !nv_port_registry_key_valid(key)) return K_EINVAL;
    if (type == NV_PORT_REGISTRY_STRING &&
        (string == 0 || nv_port_registry_string_length(
            string, NV_PORT_REGISTRY_STRING_MAX) >= NV_PORT_REGISTRY_STRING_MAX)) {
        return K_EINVAL;
    }
    uint64_t flags = nv_port_spin_lock_irqsave(&registry->lock);
    int32_t slot = nv_port_registry_find_locked(registry, key);
    if (slot < 0) slot = nv_port_registry_free_locked(registry);
    if (slot < 0) {
        nv_port_spin_unlock_irqrestore(&registry->lock, flags);
        return K_ENOSPC;
    }
    nv_port_registry_entry_t *entry = &registry->entries[(uint32_t)slot];
    if (!entry->present) {
        nv_port_registry_copy(entry->key, key, sizeof(entry->key));
        entry->present = true;
    }
    entry->type = type;
    if (type == NV_PORT_REGISTRY_U32) entry->value.u32 = u32;
    else if (type == NV_PORT_REGISTRY_U64) entry->value.u64 = u64;
    else nv_port_registry_copy(entry->value.string, string,
                               sizeof(entry->value.string));
    nv_port_spin_unlock_irqrestore(&registry->lock, flags);
    return K_OK;
}

kstatus_t nv_port_registry_set_u32(nv_port_registry_t *registry,
                                   const char *key, uint32_t value) {
    return nv_port_registry_set(registry, key, NV_PORT_REGISTRY_U32, value, 0U,
                                0);
}

kstatus_t nv_port_registry_set_u64(nv_port_registry_t *registry,
                                   const char *key, uint64_t value) {
    return nv_port_registry_set(registry, key, NV_PORT_REGISTRY_U64, 0U, value,
                                0);
}

kstatus_t nv_port_registry_set_string(nv_port_registry_t *registry,
                                      const char *key, const char *value) {
    return nv_port_registry_set(registry, key, NV_PORT_REGISTRY_STRING, 0U, 0U,
                                value);
}

static kstatus_t nv_port_registry_get_entry(nv_port_registry_t *registry,
                                             const char *key, uint32_t type,
                                             nv_port_registry_entry_t *out) {
    if (registry == 0 || !registry->initialized || out == 0 ||
        !nv_port_registry_key_valid(key)) return K_EINVAL;
    uint64_t flags = nv_port_spin_lock_irqsave(&registry->lock);
    int32_t slot = nv_port_registry_find_locked(registry, key);
    if (slot < 0) {
        nv_port_spin_unlock_irqrestore(&registry->lock, flags);
        return K_ENOENT;
    }
    if (registry->entries[(uint32_t)slot].type != type) {
        nv_port_spin_unlock_irqrestore(&registry->lock, flags);
        return K_EINVAL;
    }
    *out = registry->entries[(uint32_t)slot];
    nv_port_spin_unlock_irqrestore(&registry->lock, flags);
    return K_OK;
}

kstatus_t nv_port_registry_get_u32(nv_port_registry_t *registry,
                                   const char *key, uint32_t *value) {
    nv_port_registry_entry_t entry;
    if (value == 0) return K_EINVAL;
    kstatus_t status = nv_port_registry_get_entry(registry, key,
                                                  NV_PORT_REGISTRY_U32, &entry);
    if (status == K_OK) *value = entry.value.u32;
    return status;
}

kstatus_t nv_port_registry_get_u64(nv_port_registry_t *registry,
                                   const char *key, uint64_t *value) {
    nv_port_registry_entry_t entry;
    if (value == 0) return K_EINVAL;
    kstatus_t status = nv_port_registry_get_entry(registry, key,
                                                  NV_PORT_REGISTRY_U64, &entry);
    if (status == K_OK) *value = entry.value.u64;
    return status;
}

kstatus_t nv_port_registry_get_string(nv_port_registry_t *registry,
                                      const char *key, char *value,
                                      size_t value_capacity) {
    nv_port_registry_entry_t entry;
    if (value == 0 || value_capacity == 0U) return K_EINVAL;
    kstatus_t status = nv_port_registry_get_entry(registry, key,
                                                  NV_PORT_REGISTRY_STRING,
                                                  &entry);
    if (status != K_OK) return status;
    size_t length = nv_port_registry_string_length(entry.value.string,
                                                    sizeof(entry.value.string));
    if (length >= value_capacity) return K_ENOSPC;
    nv_port_registry_copy(value, entry.value.string, value_capacity);
    return K_OK;
}

kstatus_t nv_port_registry_remove(nv_port_registry_t *registry,
                                  const char *key) {
    if (registry == 0 || !registry->initialized ||
        !nv_port_registry_key_valid(key)) return K_EINVAL;
    uint64_t flags = nv_port_spin_lock_irqsave(&registry->lock);
    int32_t slot = nv_port_registry_find_locked(registry, key);
    if (slot < 0) {
        nv_port_spin_unlock_irqrestore(&registry->lock, flags);
        return K_ENOENT;
    }
    registry->entries[(uint32_t)slot] = (nv_port_registry_entry_t){0};
    nv_port_spin_unlock_irqrestore(&registry->lock, flags);
    return K_OK;
}

bool nv_port_registry_self_test(void) {
    nv_port_registry_t registry = {0};
    uint32_t u32 = 0U;
    uint64_t u64 = 0U;
    char string[16] = {0};

    nv_port_registry_init(&registry);
    if (nv_port_registry_set_u32(&registry, "gpu/count", 7U) != K_OK ||
        nv_port_registry_set_u64(&registry, "gpu/base", 0x100000000ULL) !=
            K_OK ||
        nv_port_registry_set_string(&registry, "gpu/name", "GB205") != K_OK ||
        nv_port_registry_get_u32(&registry, "gpu/count", &u32) != K_OK ||
        nv_port_registry_get_u64(&registry, "gpu/base", &u64) != K_OK ||
        nv_port_registry_get_string(&registry, "gpu/name", string,
                                    sizeof(string)) != K_OK ||
        u32 != 7U || u64 != 0x100000000ULL || string[0] != 'G' ||
        nv_port_registry_get_u64(&registry, "gpu/count", &u64) != K_EINVAL ||
        nv_port_registry_get_u32(&registry, "gpu/missing", &u32) != K_ENOENT ||
        nv_port_registry_remove(&registry, "gpu/count") != K_OK ||
        nv_port_registry_remove(&registry, "gpu/count") != K_ENOENT) return false;
    return true;
}
