#include "nv_port.h"

static bool nv_port_firmware_blob_valid(const firmware_blob_t *blob,
                                        uint64_t minimum_version) {
    if (blob == 0 || blob->data == 0 || blob->size == 0U ||
        blob->version < minimum_version || blob->version == 0U ||
        blob->generation == 0U) return false;
    return true;
}

kstatus_t nv_port_firmware_request(firmware_manager_t *manager,
                                   const char *logical_name,
                                   uint64_t minimum_version,
                                   nv_port_firmware_t *out) {
    if (manager == 0 || out == 0 ||
        !firmware_logical_name_valid(logical_name)) return K_EINVAL;
    *out = (nv_port_firmware_t){0};
    kstatus_t status = firmware_request(manager, logical_name, minimum_version,
                                        &out->blob);
    if (status != K_OK) return status;
    if (!nv_port_firmware_blob_valid(&out->blob, minimum_version)) {
        firmware_release(manager, &out->blob);
        return K_EIO;
    }
    out->manager = manager;
    out->acquired = true;
    return K_OK;
}

void nv_port_firmware_release(nv_port_firmware_t *firmware) {
    if (firmware == 0) return;
    if (firmware->acquired && firmware->manager != 0) {
        firmware_release(firmware->manager, &firmware->blob);
    }
    *firmware = (nv_port_firmware_t){0};
}

typedef struct nv_port_firmware_test_context {
    uint32_t releases;
} nv_port_firmware_test_context_t;

static const uint8_t g_nv_port_firmware_test_data[] = {
    0x4EU, 0x56U, 0x2DU, 0x50U, 0x4FU, 0x52U, 0x54U,
};

static kstatus_t nv_port_firmware_test_resolve(
    const char *logical_name, uint64_t minimum_version, firmware_blob_t *out,
    void *context) {
    (void)context;
    if (logical_name == 0 || out == 0 ||
        !firmware_logical_name_valid(logical_name) ||
        logical_name[0] != 'g' || minimum_version > 7U) return K_ENOENT;
    out->data = g_nv_port_firmware_test_data;
    out->size = sizeof(g_nv_port_firmware_test_data);
    out->version = 7U;
    out->generation = 1U;
    out->package_id[0] = 0x42U;
    return K_OK;
}

static void nv_port_firmware_test_release(firmware_blob_t *blob,
                                          void *context) {
    nv_port_firmware_test_context_t *test =
        (nv_port_firmware_test_context_t *)context;
    if (test != 0) ++test->releases;
    if (blob != 0) *blob = (firmware_blob_t){0};
}

bool nv_port_firmware_self_test(void) {
    firmware_manager_t manager = {0};
    nv_port_firmware_t firmware = {0};
    nv_port_firmware_test_context_t context = {0};
    if (firmware_manager_init(&manager, nv_port_firmware_test_resolve,
                              nv_port_firmware_test_release, &context) != K_OK ||
        nv_port_firmware_request(&manager, "gpu/gb205.bin", 7U,
                                 &firmware) != K_OK ||
        !firmware.acquired || firmware.blob.size !=
            sizeof(g_nv_port_firmware_test_data)) return false;
    nv_port_firmware_release(&firmware);
    return context.releases == 1U && !firmware.acquired &&
           nv_port_firmware_request(&manager, "gpu/gb205.bin", 8U,
                                    &firmware) == K_ENOENT;
}
