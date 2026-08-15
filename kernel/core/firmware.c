#include <kernel/firmware.h>

static void firmware_lock(firmware_manager_t *manager) {
    while (atomic_exchange_explicit(&manager->lock.state, 1U,
                                    memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
}

static void firmware_unlock(firmware_manager_t *manager) {
    atomic_store_explicit(&manager->lock.state, 0U, memory_order_release);
}

static bool firmware_byte_is_name_char(char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '_' || value == '-' ||
           value == '.' || value == '/' ;
}

bool firmware_logical_name_valid(const char *logical_name) {
    uint32_t length = 0U;
    uint32_t component_length = 0U;
    if (logical_name == 0 || logical_name[0] == '/' || logical_name[0] == '\0') {
        return false;
    }
    for (; length < FIRMWARE_NAME_CAP; ++length) {
        char value = logical_name[length];
        if (value == '\0') break;
        if (!firmware_byte_is_name_char(value)) return false;
        if (value == '/') {
            if (component_length == 0U) return false;
            component_length = 0U;
        } else {
            ++component_length;
        }
    }
    if (length == 0U || length == FIRMWARE_NAME_CAP || component_length == 0U) return false;
    /* 禁止把路径逃逸语义交给底层 provider。 */
    for (uint32_t i = 0U; i + 1U < length; ++i) {
        if (logical_name[i] == '.' && logical_name[i + 1U] == '.' &&
            (i == 0U || logical_name[i - 1U] == '/') &&
            (i + 2U == length || logical_name[i + 2U] == '/')) return false;
    }
    return true;
}

static bool firmware_bytes_nonzero(const uint8_t *bytes, uint32_t length) {
    uint8_t difference = 0U;
    if (bytes == 0) return false;
    for (uint32_t i = 0U; i < length; ++i) difference |= bytes[i];
    return difference != 0U;
}

static bool firmware_blob_valid(const firmware_blob_t *blob,
                                uint64_t minimum_version) {
    uint8_t digest[KERNEL_SHA256_DIGEST_SIZE];
    if (blob == 0 || blob->data == 0 || blob->size == 0U ||
        blob->version == 0U || blob->version < minimum_version ||
        blob->generation == 0U ||
        !firmware_bytes_nonzero(blob->package_id, FIRMWARE_PACKAGE_ID_SIZE) ||
        !firmware_bytes_nonzero(blob->digest, KERNEL_SHA256_DIGEST_SIZE)) return false;
    kernel_sha256_compute(blob->data, blob->size, digest);
    return kernel_sha256_equal(digest, blob->digest);
}

kstatus_t firmware_manager_init(firmware_manager_t *manager,
                                firmware_resolve_fn resolve,
                                firmware_release_fn release,
                                void *context) {
    unsigned expected = 0U;
    if (manager == 0 || resolve == 0 || release == 0) return K_EINVAL;
    if (atomic_compare_exchange_strong_explicit(&manager->init_state, &expected, 1U,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        atomic_init(&manager->lock.state, 0U);
        manager->resolve = resolve;
        manager->release = release;
        manager->context = context;
        atomic_store_explicit(&manager->init_state, 2U, memory_order_release);
        return K_OK;
    }
    while (atomic_load_explicit(&manager->init_state, memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
    return manager->resolve == resolve && manager->release == release ? K_OK : K_EBUSY;
}

kstatus_t firmware_request(firmware_manager_t *manager,
                           const char *logical_name,
                           uint64_t minimum_version,
                           firmware_blob_t *out) {
    kstatus_t status;
    firmware_resolve_fn resolve;
    firmware_release_fn release;
    void *context;
    if (manager == 0 || out == 0 || !firmware_logical_name_valid(logical_name) ||
        atomic_load_explicit(&manager->init_state, memory_order_acquire) != 2U) {
        return K_EINVAL;
    }
    *out = (firmware_blob_t){0};
    /* 供应者可能访问文件系统或等待 I/O，不能在自旋锁内执行外部回调。 */
    firmware_lock(manager);
    resolve = manager->resolve;
    release = manager->release;
    context = manager->context;
    firmware_unlock(manager);
    status = resolve(logical_name, minimum_version, out, context);
    if (status != K_OK) return status;
    if (!firmware_blob_valid(out, minimum_version)) {
        release(out, context);
        *out = (firmware_blob_t){0};
        return K_EIO;
    }
    return K_OK;
}

void firmware_release(firmware_manager_t *manager, firmware_blob_t *blob) {
    firmware_release_fn release;
    void *context;
    if (manager == 0 || blob == 0 || blob->data == 0 ||
        atomic_load_explicit(&manager->init_state, memory_order_acquire) != 2U) return;
    firmware_lock(manager);
    release = manager->release;
    context = manager->context;
    firmware_unlock(manager);
    release(blob, context);
    *blob = (firmware_blob_t){0};
}

typedef struct firmware_test_source {
    uint32_t releases;
} firmware_test_source_t;

static const uint8_t g_firmware_test_image[] = "LiteOS test firmware";

static kstatus_t firmware_test_resolve(const char *logical_name,
                                       uint64_t minimum_version,
                                       firmware_blob_t *out,
                                       void *context) {
    (void)context;
    if (logical_name == 0 || out == 0 || minimum_version > 2U) return K_ENOENT;
    if (logical_name[0] != 'w' || logical_name[1] != 'i') return K_ENOENT;
    out->data = g_firmware_test_image;
    out->size = sizeof(g_firmware_test_image) - 1U;
    out->version = 2U;
    out->generation = 7U;
    out->package_id[0] = 0x11U;
    kernel_sha256_compute(out->data, out->size, out->digest);
    return K_OK;
}

static void firmware_test_release(firmware_blob_t *blob, void *context) {
    firmware_test_source_t *source = (firmware_test_source_t *)context;
    if (source != 0) ++source->releases;
    if (blob != 0) blob->data = 0;
}

bool firmware_core_self_test(void) {
    firmware_test_source_t source = {0};
    firmware_manager_t manager = {0};
    firmware_blob_t blob = {0};
    if (!kernel_sha256_self_test() ||
        firmware_manager_init(&manager, firmware_test_resolve,
                              firmware_test_release, &source) != K_OK ||
        !firmware_logical_name_valid("wifi/rtl.bin") ||
        firmware_logical_name_valid("../rtl.bin") ||
        firmware_logical_name_valid("wifi//rtl.bin") ||
        firmware_request(&manager, "wifi/rtl.bin", 2U, &blob) != K_OK ||
        blob.version != 2U || blob.generation != 7U || blob.size == 0U) {
        return false;
    }
    firmware_release(&manager, &blob);
    if (source.releases != 1U || blob.data != 0 ||
        firmware_request(&manager, "wifi/rtl.bin", 3U, &blob) != K_ENOENT ||
        firmware_request(&manager, "../rtl.bin", 0U, &blob) != K_EINVAL) return false;
    return true;
}
