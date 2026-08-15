#include <kernel/kmem.h>
#include <kernel/spinlock.h>
#include <kernel/update.h>
#include <rsa.h>

struct update_manager {
    spinlock_t lock;
    update_signature_verify_fn verify;
    update_state_load_fn load;
    update_state_store_fn store;
    void *context;
    update_state_t state;
};

static void update_zero(void *memory, size_t size) {
    uint8_t *bytes = (uint8_t *)memory;
    while (size-- != 0U) *bytes++ = 0U;
}

static void update_lock(update_manager_t *manager) {
    while (atomic_exchange_explicit(&manager->lock.state, 1U,
                                    memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
}

static void update_unlock(update_manager_t *manager) {
    atomic_store_explicit(&manager->lock.state, 0U, memory_order_release);
}

static update_state_t update_default_state(void) {
    update_state_t state = {0};
    state.magic = UPDATE_STATE_MAGIC;
    state.version = UPDATE_MANIFEST_VERSION;
    state.active_slot = UPDATE_SLOT_A;
    state.pending_slot = UPDATE_SLOT_NONE;
    state.boot_attempts = 0U;
    state.safe_mode = 0U;
    return state;
}

static bool update_state_valid(const update_state_t *state) {
    if (state == 0 || state->magic != UPDATE_STATE_MAGIC ||
        state->version != UPDATE_MANIFEST_VERSION ||
        (state->active_slot != UPDATE_SLOT_A && state->active_slot != UPDATE_SLOT_B) ||
        (state->pending_slot != UPDATE_SLOT_NONE &&
         state->pending_slot != UPDATE_SLOT_A && state->pending_slot != UPDATE_SLOT_B) ||
        state->boot_attempts > UPDATE_MAX_BOOT_ATTEMPTS || state->safe_mode > 1U) {
        return false;
    }
    if (state->pending_slot == UPDATE_SLOT_NONE &&
        (state->pending_version != 0U || state->pending_generation != 0U)) return false;
    if ((state->confirmed_version == 0U) != (state->generation == 0U)) return false;
    if (state->pending_slot != UPDATE_SLOT_NONE &&
        (state->pending_version == 0U || state->pending_generation == 0U ||
         state->pending_generation <= state->generation)) return false;
    return true;
}

static kstatus_t update_store_locked(update_manager_t *manager) {
    if (manager->store == 0) return K_OK;
    return manager->store(&manager->state, manager->context);
}

static bool update_package_id_valid(const uint8_t package_id[UPDATE_PACKAGE_ID_SIZE]) {
    uint8_t difference = 0U;
    for (uint32_t i = 0; i < UPDATE_PACKAGE_ID_SIZE; ++i) difference |= package_id[i];
    return difference != 0U;
}

static bool update_manifest_valid(const update_manifest_t *manifest,
                                  const void *image, size_t image_size) {
    return manifest != 0 && image != 0 && image_size != 0U &&
           manifest->format_version == UPDATE_MANIFEST_VERSION &&
           (manifest->slot == UPDATE_SLOT_A || manifest->slot == UPDATE_SLOT_B) &&
           manifest->version != 0U && manifest->generation != 0U &&
           manifest->image_size == image_size &&
           update_package_id_valid(manifest->package_id);
}

kstatus_t update_manager_create(update_signature_verify_fn verify,
                                update_state_load_fn load,
                                update_state_store_fn store, void *context,
                                update_manager_t **out) {
    update_manager_t *manager;
    kstatus_t status;
    if (verify == 0 || out == 0) return K_EINVAL;
    manager = (update_manager_t *)kzalloc(sizeof(*manager), 0);
    if (manager == 0) return K_ENOMEM;
    atomic_init(&manager->lock.state, 0U);
    manager->verify = verify;
    manager->load = load;
    manager->store = store;
    manager->context = context;
    manager->state = update_default_state();
    if (load != 0) {
        update_state_t loaded = {0};
        status = load(&loaded, context);
        if (status == K_OK) {
            if (!update_state_valid(&loaded)) {
                kfree(manager);
                return K_EIO;
            }
            manager->state = loaded;
        } else if (status != K_ENOENT) {
            kfree(manager);
            return status;
        }
    }
    *out = manager;
    return K_OK;
}

void update_manager_destroy(update_manager_t *manager) {
    if (manager != 0) kfree(manager);
}

kstatus_t update_stage(update_manager_t *manager, const update_manifest_t *manifest,
                       const void *image, size_t image_size) {
    uint8_t digest[KERNEL_SHA256_DIGEST_SIZE];
    update_state_t previous;
    if (manager == 0 || !update_manifest_valid(manifest, image, image_size)) {
        return K_EINVAL;
    }
    kernel_sha256_compute(image, image_size, digest);
    if (!kernel_sha256_equal(digest, manifest->digest) ||
        !manager->verify(manifest, image, image_size, manager->context)) {
        return K_EACCES;
    }
    update_lock(manager);
    previous = manager->state;
    if (manifest->slot == manager->state.active_slot ||
        manifest->version <= manager->state.confirmed_version ||
        (manager->state.pending_slot != UPDATE_SLOT_NONE &&
         manifest->version <= manager->state.pending_version) ||
        manifest->generation <= manager->state.generation) {
        update_unlock(manager);
        return K_EINVAL;
    }
    manager->state.pending_slot = manifest->slot;
    manager->state.pending_version = manifest->version;
    manager->state.pending_generation = manifest->generation;
    manager->state.boot_attempts = 0U;
    manager->state.safe_mode = 0U;
    kstatus_t status = update_store_locked(manager);
    if (status != K_OK) manager->state = previous;
    update_unlock(manager);
    return status;
}

kstatus_t update_select_boot(update_manager_t *manager, update_slot_t *slot) {
    kstatus_t status = K_OK;
    update_state_t previous;
    update_slot_t selected;
    if (manager == 0 || slot == 0) return K_EINVAL;
    update_lock(manager);
    previous = manager->state;
    if (manager->state.safe_mode || manager->state.pending_slot == UPDATE_SLOT_NONE) {
        selected = (update_slot_t)manager->state.active_slot;
        update_unlock(manager);
        *slot = selected;
        return K_OK;
    }
    if (manager->state.boot_attempts >= UPDATE_MAX_BOOT_ATTEMPTS) {
        manager->state.safe_mode = 1U;
        manager->state.pending_slot = UPDATE_SLOT_NONE;
        manager->state.pending_version = 0U;
        manager->state.pending_generation = 0U;
        selected = (update_slot_t)manager->state.active_slot;
    } else {
        ++manager->state.boot_attempts;
        selected = (update_slot_t)manager->state.pending_slot;
    }
    status = update_store_locked(manager);
    if (status != K_OK) {
        manager->state = previous;
        update_unlock(manager);
        return status;
    }
    update_unlock(manager);
    *slot = selected;
    return status;
}

kstatus_t update_mark_boot_success(update_manager_t *manager) {
    update_state_t previous;
    if (manager == 0) return K_EINVAL;
    update_lock(manager);
    if (manager->state.pending_slot == UPDATE_SLOT_NONE) {
        update_unlock(manager);
        return K_ENOENT;
    }
    previous = manager->state;
    manager->state.active_slot = manager->state.pending_slot;
    manager->state.confirmed_version = manager->state.pending_version;
    manager->state.generation = manager->state.pending_generation;
    manager->state.pending_slot = UPDATE_SLOT_NONE;
    manager->state.pending_version = 0U;
    manager->state.pending_generation = 0U;
    manager->state.boot_attempts = 0U;
    manager->state.safe_mode = 0U;
    kstatus_t status = update_store_locked(manager);
    if (status != K_OK) manager->state = previous;
    update_unlock(manager);
    return status;
}

kstatus_t update_mark_boot_failure(update_manager_t *manager) {
    update_state_t previous;
    if (manager == 0) return K_EINVAL;
    update_lock(manager);
    if (manager->state.pending_slot == UPDATE_SLOT_NONE) {
        update_unlock(manager);
        return K_ENOENT;
    }
    /* boot_attempts 已在选择 pending slot 时递增，这里只根据已记录的
       启动次数决定是否回滚，避免一次启动被重复计数。 */
    previous = manager->state;
    if (manager->state.boot_attempts >= UPDATE_MAX_BOOT_ATTEMPTS) {
        manager->state.safe_mode = 1U;
        manager->state.pending_slot = UPDATE_SLOT_NONE;
        manager->state.pending_version = 0U;
        manager->state.pending_generation = 0U;
    }
    kstatus_t status = update_store_locked(manager);
    if (status != K_OK) manager->state = previous;
    update_unlock(manager);
    return status;
}

kstatus_t update_enter_safe_mode(update_manager_t *manager) {
    update_state_t previous;
    if (manager == 0) return K_EINVAL;
    update_lock(manager);
    previous = manager->state;
    manager->state.safe_mode = 1U;
    manager->state.pending_slot = UPDATE_SLOT_NONE;
    manager->state.pending_version = 0U;
    manager->state.pending_generation = 0U;
    kstatus_t status = update_store_locked(manager);
    if (status != K_OK) manager->state = previous;
    update_unlock(manager);
    return status;
}

kstatus_t update_clear_safe_mode(update_manager_t *manager) {
    update_state_t previous;
    if (manager == 0) return K_EINVAL;
    update_lock(manager);
    previous = manager->state;
    manager->state.safe_mode = 0U;
    manager->state.boot_attempts = 0U;
    kstatus_t status = update_store_locked(manager);
    if (status != K_OK) manager->state = previous;
    update_unlock(manager);
    return status;
}

kstatus_t update_get_state(update_manager_t *manager, update_state_t *state) {
    if (manager == 0 || state == 0) return K_EINVAL;
    update_lock(manager);
    *state = manager->state;
    update_unlock(manager);
    return K_OK;
}

typedef struct update_test_storage {
    update_state_t state;
    bool valid;
    bool fail_store;
} update_test_storage_t;

static kstatus_t update_test_load(update_state_t *state, void *context) {
    update_test_storage_t *storage = (update_test_storage_t *)context;
    if (storage == 0 || state == 0) return K_EINVAL;
    if (!storage->valid) return K_ENOENT;
    *state = storage->state;
    return K_OK;
}

static kstatus_t update_test_store(const update_state_t *state, void *context) {
    update_test_storage_t *storage = (update_test_storage_t *)context;
    if (storage == 0 || state == 0) return K_EINVAL;
    if (storage->fail_store) return K_EIO;
    storage->state = *state;
    storage->valid = true;
    return K_OK;
}

static bool update_test_verify(const update_manifest_t *manifest, const void *image,
                               size_t image_size, void *context) {
    (void)image;
    (void)image_size;
    (void)context;
    /* 自检直接走与 Loader 相同的 RSA-2048 PKCS#1 v1.5 信任根。 */
    return manifest != 0 && rsa2048_sha256_verify(
        manifest->digest, manifest->signature) != 0;
}

bool update_core_self_test(void) {
    static const uint8_t image[] = "LiteOS signed update image";
    static const uint8_t signature[UPDATE_SIGNATURE_SIZE] = {
        0x7B,0x97,0x3B,0x72,0x92,0x13,0x95,0x90,0xC6,0x32,0x3E,0x0C,0x6C,0x32,0xAB,0xE2,
        0x9E,0xE7,0x1E,0xFD,0x0F,0x42,0xFA,0x31,0xC3,0x4F,0x5D,0xAE,0xDC,0x6F,0xD3,0x58,
        0x92,0x51,0xF6,0xC0,0x40,0x3D,0xFD,0xD9,0x40,0x5D,0x6F,0xED,0x84,0x22,0xF2,0xCA,
        0xC2,0xC9,0xC0,0x32,0x6D,0x80,0xA5,0xF9,0x9B,0x2C,0xE7,0x81,0xD2,0x9A,0xE6,0xB8,
        0xF4,0xA4,0x7F,0xDC,0xF4,0x7A,0x68,0xFA,0x38,0x0C,0x46,0xC6,0xB0,0x63,0x37,0x75,
        0x72,0x4E,0xDD,0x81,0x0A,0x0B,0xF2,0x23,0xD8,0xFD,0x4E,0x53,0x51,0x45,0x43,0xE1,
        0x6C,0x1D,0x71,0xEF,0x08,0x3A,0xFE,0x04,0x7C,0xE6,0xD9,0x0F,0x1B,0xB0,0xD7,0xE3,
        0xED,0x54,0x8A,0x1B,0x0C,0xAC,0x66,0xCF,0xB9,0x17,0xDB,0x6C,0xBD,0x42,0x2F,0x2E,
        0xF3,0x81,0x26,0x91,0x71,0x71,0xEE,0xDB,0x3B,0xD4,0xA1,0x54,0xFA,0xB3,0x0A,0x16,
        0xBD,0x73,0xC7,0x7F,0x13,0xDE,0x28,0x49,0xA1,0xBB,0xCA,0xBF,0x47,0x79,0x72,0x83,
        0x5C,0x59,0x14,0xC2,0x07,0xF3,0x35,0x1E,0xC9,0xE4,0xBA,0x21,0x20,0x9F,0xA0,0x42,
        0x6E,0x51,0xEF,0xC9,0x60,0x86,0x2A,0x10,0x9F,0xE7,0x77,0xA4,0x94,0x18,0x72,0x2C,
        0x01,0x59,0x77,0xBE,0x1A,0xA7,0x1E,0xBC,0xF1,0x38,0x73,0x86,0x78,0x64,0x26,0x3D,
        0x57,0xFF,0xF2,0x4D,0x02,0x69,0x60,0x20,0xEA,0x89,0xA8,0x94,0xB9,0x6F,0x28,0x32,
        0x73,0x4D,0xB6,0xD1,0x2D,0x81,0x67,0xB5,0x0B,0x69,0x42,0x6E,0x5E,0x75,0xE4,0x8D,
        0xD1,0xB7,0x06,0xFD,0x2C,0xBF,0xBC,0xF8,0x39,0xAB,0x24,0xB6,0x7D,0xD2,0xAB,0x67
    };
    update_test_storage_t storage = {0};
    update_manager_t *manager = 0;
    update_manifest_t manifest;
    update_state_t state = {0};
    update_slot_t slot = UPDATE_SLOT_NONE;
    uint8_t original_digest[KERNEL_SHA256_DIGEST_SIZE];
    bool success;
    update_zero(&manifest, sizeof(manifest));
    if (!kernel_sha256_self_test()) return false;
    manifest.format_version = UPDATE_MANIFEST_VERSION;
    manifest.slot = UPDATE_SLOT_B;
    manifest.version = 1U;
    manifest.generation = 1U;
    manifest.image_size = sizeof(image);
    manifest.package_id[0] = 1U;
    for (uint32_t i = 0; i < UPDATE_SIGNATURE_SIZE; ++i) {
        manifest.signature[i] = signature[i];
    }
    kernel_sha256_compute(image, sizeof(image), manifest.digest);
    for (uint32_t i = 0; i < KERNEL_SHA256_DIGEST_SIZE; ++i) original_digest[i] = manifest.digest[i];
    if (update_manager_create(update_test_verify, update_test_load, update_test_store,
                              &storage, &manager) != K_OK || manager == 0 ||
        update_stage(manager, &manifest, image, sizeof(image)) != K_OK ||
        update_select_boot(manager, &slot) != K_OK || slot != UPDATE_SLOT_B ||
        update_mark_boot_failure(manager) != K_OK ||
        update_select_boot(manager, &slot) != K_OK || slot != UPDATE_SLOT_B ||
        update_mark_boot_failure(manager) != K_OK ||
        update_select_boot(manager, &slot) != K_OK || slot != UPDATE_SLOT_B ||
        update_mark_boot_failure(manager) != K_OK ||
        update_select_boot(manager, &slot) != K_OK || slot != UPDATE_SLOT_A ||
        update_get_state(manager, &state) != K_OK || state.safe_mode == 0U ||
        update_clear_safe_mode(manager) != K_OK) {
        update_manager_destroy(manager);
        return false;
    }
    manifest.version = 2U;
    manifest.generation = 2U;
    manifest.slot = UPDATE_SLOT_B;
    manifest.signature[0] = 0U;
    if (update_stage(manager, &manifest, image, sizeof(image)) != K_EACCES) {
        update_manager_destroy(manager);
        return false;
    }
    for (uint32_t i = 0; i < UPDATE_SIGNATURE_SIZE; ++i) {
        manifest.signature[i] = signature[i];
    }
    manifest.digest[0] ^= 1U;
    if (update_stage(manager, &manifest, image, sizeof(image)) != K_EACCES) {
        update_manager_destroy(manager);
        return false;
    }
    for (uint32_t i = 0; i < KERNEL_SHA256_DIGEST_SIZE; ++i) manifest.digest[i] = original_digest[i];
    if (update_stage(manager, &manifest, image, sizeof(image)) != K_OK ||
        update_select_boot(manager, &slot) != K_OK || slot != UPDATE_SLOT_B ||
        update_mark_boot_success(manager) != K_OK || update_get_state(manager, &state) != K_OK) {
        update_manager_destroy(manager);
        return false;
    }
    manifest.version = 3U;
    manifest.generation = 3U;
    manifest.slot = UPDATE_SLOT_A;
    storage.fail_store = true;
    if (update_stage(manager, &manifest, image, sizeof(image)) != K_EIO) {
        update_manager_destroy(manager);
        return false;
    }
    storage.fail_store = false;
    if (update_get_state(manager, &state) != K_OK ||
        state.active_slot != UPDATE_SLOT_B ||
        state.pending_slot != UPDATE_SLOT_NONE || state.confirmed_version != 2U) {
        update_manager_destroy(manager);
        return false;
    }
    success = state.active_slot == UPDATE_SLOT_B && state.confirmed_version == 2U &&
              state.pending_slot == UPDATE_SLOT_NONE && state.safe_mode == 0U &&
              storage.valid;
    update_manager_destroy(manager);
    return success;
}
