#include <kernel/package.h>

static void package_lock(package_registry_t *registry) {
    while (atomic_exchange_explicit(&registry->lock.state, 1U,
                                    memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
}

static void package_unlock(package_registry_t *registry) {
    atomic_store_explicit(&registry->lock.state, 0U, memory_order_release);
}

static bool package_id_equal(const uint8_t left[PACKAGE_ID_SIZE],
                             const uint8_t right[PACKAGE_ID_SIZE]) {
    uint8_t difference = 0U;
    for (uint32_t i = 0; i < PACKAGE_ID_SIZE; ++i) difference |= (uint8_t)(left[i] ^ right[i]);
    return difference == 0U;
}

static bool package_bytes_nonzero(const uint8_t *bytes, uint32_t length) {
    uint8_t difference = 0U;
    for (uint32_t i = 0; i < length; ++i) difference |= bytes[i];
    return difference != 0U;
}

bool package_identity_valid(const package_identity_t *identity) {
    if (identity == 0 || !package_bytes_nonzero(identity->package_id, PACKAGE_ID_SIZE) ||
        !package_bytes_nonzero(identity->payload_digest, PACKAGE_DIGEST_SIZE) ||
        identity->version == 0U || identity->generation == 0U || identity->reserved != 0U) {
        return false;
    }
    bool has_name = false;
    for (uint32_t i = 0; i < PACKAGE_NAME_CAP; ++i) {
        if (identity->name[i] == '\0') {
            has_name = i != 0U;
            break;
        }
    }
    return has_name;
}

bool package_registry_init(package_registry_t *registry) {
    unsigned expected = 0U;
    if (registry == 0) return false;
    if (atomic_compare_exchange_strong_explicit(&registry->init_state, &expected, 1U,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        atomic_init(&registry->lock.state, 0U);
        for (uint32_t i = 0; i < PACKAGE_REGISTRY_CAPACITY; ++i) {
            registry->entries[i].registered = false;
            registry->entries[i].identity = (package_identity_t){0};
        }
        atomic_store_explicit(&registry->init_state, 2U, memory_order_release);
        return true;
    }
    while (atomic_load_explicit(&registry->init_state, memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
    return true;
}

static package_registry_entry_t *package_find_locked(
    package_registry_t *registry, const uint8_t package_id[PACKAGE_ID_SIZE]) {
    for (uint32_t i = 0; i < PACKAGE_REGISTRY_CAPACITY; ++i) {
        if (registry->entries[i].registered &&
            package_id_equal(registry->entries[i].identity.package_id, package_id)) {
            return &registry->entries[i];
        }
    }
    return 0;
}

kstatus_t package_register(package_registry_t *registry,
                           const package_identity_t *identity) {
    package_registry_entry_t *entry;
    if (!package_registry_init(registry) || !package_identity_valid(identity)) return K_EINVAL;
    package_lock(registry);
    entry = package_find_locked(registry, identity->package_id);
    if (entry != 0) {
        /* 同一身份只允许单调升级，防止回滚包覆盖已确认版本。 */
        if (identity->version <= entry->identity.version ||
            identity->generation <= entry->identity.generation) {
            package_unlock(registry);
            return K_EINVAL;
        }
        entry->identity = *identity;
        package_unlock(registry);
        return K_OK;
    }
    for (uint32_t i = 0; i < PACKAGE_REGISTRY_CAPACITY; ++i) {
        if (!registry->entries[i].registered) {
            registry->entries[i].identity = *identity;
            registry->entries[i].registered = true;
            package_unlock(registry);
            return K_OK;
        }
    }
    package_unlock(registry);
    return K_ENOMEM;
}

kstatus_t package_lookup(package_registry_t *registry,
                         const uint8_t package_id[PACKAGE_ID_SIZE],
                         package_identity_t *identity) {
    package_registry_entry_t *entry;
    if (!package_registry_init(registry) || package_id == 0 || identity == 0 ||
        !package_bytes_nonzero(package_id, PACKAGE_ID_SIZE)) return K_EINVAL;
    package_lock(registry);
    entry = package_find_locked(registry, package_id);
    if (entry == 0) {
        package_unlock(registry);
        return K_ENOENT;
    }
    *identity = entry->identity;
    package_unlock(registry);
    return K_OK;
}

kstatus_t package_unregister(package_registry_t *registry,
                             const uint8_t package_id[PACKAGE_ID_SIZE]) {
    package_registry_entry_t *entry;
    if (!package_registry_init(registry) || package_id == 0 ||
        !package_bytes_nonzero(package_id, PACKAGE_ID_SIZE)) return K_EINVAL;
    package_lock(registry);
    entry = package_find_locked(registry, package_id);
    if (entry == 0) {
        package_unlock(registry);
        return K_ENOENT;
    }
    entry->registered = false;
    entry->identity = (package_identity_t){0};
    package_unlock(registry);
    return K_OK;
}

bool package_core_self_test(void) {
    static package_registry_t registry;
    package_identity_t identity = {0};
    package_identity_t found = {0};
    identity.package_id[0] = 0x42U;
    identity.payload_digest[0] = 0xA5U;
    identity.version = 1U;
    identity.generation = 1U;
    identity.owner_uid = 0U;
    identity.owner_gid = 0U;
    identity.name[0] = 's';
    identity.name[1] = 'y';
    identity.name[2] = 's';
    identity.name[3] = 't';
    identity.name[4] = 'e';
    identity.name[5] = 'm';
    if (!package_registry_init(&registry) ||
        package_register(&registry, &identity) != K_OK ||
        package_lookup(&registry, identity.package_id, &found) != K_OK ||
        found.version != 1U || package_register(&registry, &identity) != K_EINVAL) return false;
    identity.version = 2U;
    identity.generation = 2U;
    if (package_register(&registry, &identity) != K_OK ||
        package_unregister(&registry, identity.package_id) != K_OK ||
        package_lookup(&registry, identity.package_id, &found) != K_ENOENT) return false;
    return true;
}
