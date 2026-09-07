#define NV_FIRMWARE_FOR_NAME(name) name ".bin"
#include "nvabi_internal.h"

#include <boot/sha256.h>
#include <kernel/mm.h>
#include <kernel/vfs.h>

/*
 * The vendor nv_get_firmware contract returns an opaque lifetime handle and
 * a separately visible immutable byte buffer.  This adapter owns only the
 * system-memory copy read from the mounted LiteOS filesystem.  It does not
 * assign GPU addresses, create GMMU mappings, or speak GSP RPC.
 */
#define NVABI_FIRMWARE_MAGIC 0x4E564657U
#define NVABI_FIRMWARE_PATH_CAP 128U
#define NVABI_FIRMWARE_MAX_BYTES (128U * 1024U * 1024U)

typedef struct nvabi_firmware_handle {
    uint32_t magic;
    uint32_t size;
    nv_firmware_type_t type;
    nv_firmware_chip_family_t family;
    uint8_t *data;
    char path[NVABI_FIRMWARE_PATH_CAP];
} nvabi_firmware_handle_t;

static const uint8_t g_gsp_ga10x_sha256[32] = {
    0x1cU, 0x10U, 0x7dU, 0x18U, 0xcdU, 0x63U, 0xa0U, 0x8fU,
    0x4bU, 0xa7U, 0x87U, 0x06U, 0x80U, 0x98U, 0x25U, 0xc7U,
    0x56U, 0xdbU, 0x2bU, 0x4aU, 0x55U, 0x6cU, 0x45U, 0x3fU,
    0xb5U, 0xc0U, 0x03U, 0x97U, 0x68U, 0x39U, 0x94U, 0x6fU,
};

static const uint8_t g_gsp_tu10x_sha256[32] = {
    0x76U, 0xe4U, 0xd2U, 0x16U, 0xb8U, 0x0fU, 0x80U, 0xc9U,
    0x95U, 0xf9U, 0x7eU, 0x11U, 0x4eU, 0x7aU, 0xa1U, 0xb8U,
    0x74U, 0x75U, 0xf6U, 0xd6U, 0x94U, 0xcbU, 0x78U, 0xd7U,
    0x8dU, 0x2eU, 0xf5U, 0x20U, 0x70U, 0xfbU, 0x07U, 0x39U,
};

static bool nvabi_firmware_name(nv_firmware_type_t type,
                                nv_firmware_chip_family_t family,
                                const char **name,
                                const uint8_t **expected_hash) {
    const char *resolved;

    if (name == NULL || expected_hash == NULL) return false;
    *name = NULL;
    *expected_hash = NULL;
    resolved = nv_firmware_for_chip_family(type, family);
    if (resolved == NULL || resolved[0] == '\0') return false;

    if (type != NV_FIRMWARE_TYPE_GSP) {
        /* The matching 595.84 package does not ship a GSP log ELF.  The
         * vendor path treats that image as optional when logs are disabled;
         * do not substitute another binary. */
        *name = resolved;
        return false;
    }

    *name = resolved;
    if (nvabi_memcmp(resolved, "gsp_ga10x.bin", 13U) == 0)
        *expected_hash = g_gsp_ga10x_sha256;
    else if (nvabi_memcmp(resolved, "gsp_tu10x.bin", 13U) == 0)
        *expected_hash = g_gsp_tu10x_sha256;
    else
        return false;
    return true;
}

static bool nvabi_firmware_path(char *out, size_t capacity,
                                const char *root, const char *name) {
    size_t root_length;
    size_t name_length;

    if (out == NULL || root == NULL || name == NULL || capacity == 0U)
        return false;
    root_length = nvabi_strlen(root);
    name_length = nvabi_strlen(name);
    if (root_length + name_length + 1U >= capacity) return false;
    nvabi_memcpy(out, root, root_length);
    out[root_length] = '/';
    nvabi_memcpy(out + root_length + 1U, name, name_length);
    out[root_length + name_length + 1U] = '\0';
    return true;
}

static bool nvabi_firmware_hash_matches(const uint8_t *digest,
                                        const uint8_t *expected) {
    return digest != NULL && expected != NULL &&
           nvabi_memcmp(digest, expected, 32U) == 0;
}

static void nvabi_firmware_hash_text(const uint8_t digest[32], char out[65]) {
    static const char hex[] = "0123456789abcdef";
    for (size_t index = 0U; index < 32U; ++index) {
        out[index * 2U] = hex[digest[index] >> 4U];
        out[index * 2U + 1U] = hex[digest[index] & 0x0fU];
    }
    out[64] = '\0';
}

static void nvabi_firmware_fail(const char *requested,
                                nv_firmware_type_t type,
                                nv_firmware_chip_family_t family) {
    liteos_serial_printf_serial_only(
        "LITEOS_NVIDIA_FIRMWARE_FAIL type=%u family=%u requested=%s expectation=595.84\r\n",
        (unsigned)type, (unsigned)family,
        requested != NULL ? requested : "unsupported");
}

const void *NV_API_CALL nv_get_firmware(
    nv_state_t *nv,
    nv_firmware_type_t type,
    nv_firmware_chip_family_t family,
    const void **fw_buf,
    NvU32 *fw_size) {
    const char *name = NULL;
    const uint8_t *expected_hash = NULL;
    static const char *const roots[] = {
        "/EFI/LITEOS/firmware",
        "/lib/firmware",
    };
    char requested[NVABI_FIRMWARE_PATH_CAP];
    char resolved[NVABI_FIRMWARE_PATH_CAP];
    uint8_t digest[32];
    char digest_text[65];
    file_t *file = NULL;
    uint8_t *data = NULL;
    nvabi_firmware_handle_t *handle = NULL;
    uint64_t file_size;
    uint64_t bytes;
    kstatus_t status;

    (void)nv;
    if (fw_buf != NULL) *fw_buf = NULL;
    if (fw_size != NULL) *fw_size = 0U;

    if (!nvabi_firmware_name(type, family, &name, &expected_hash) ||
        fw_buf == NULL || fw_size == NULL ||
        !nvabi_firmware_path(requested, sizeof(requested), "/", name)) {
        nvabi_firmware_fail(name, type, family);
        return NULL;
    }

    /* The leading slash in the requested name is kept as a bounded metadata
     * string; the vendor name itself remains the request identity. */
    nvabi_memcpy(requested, name, nvabi_strlen(name) + 1U);
    liteos_serial_printf_serial_only(
        "LITEOS_NVIDIA_FIRMWARE_REQUEST type=%u family=%u requested=%s expectation=595.84\r\n",
        (unsigned)type, (unsigned)family, requested);

    for (size_t root_index = 0U;
         root_index < sizeof(roots) / sizeof(roots[0]); ++root_index) {
        if (!nvabi_firmware_path(resolved, sizeof(resolved),
                                 roots[root_index], name))
            continue;
        file = NULL;
        status = vfs_open_kernel(resolved, VFS_OPEN_READ, 0U, &file);
        if (status != K_OK || file == NULL || file->vnode == NULL) {
            if (file != NULL) vfs_close(file);
            file = NULL;
            continue;
        }
        file_size = file->vnode->size;
        if (file_size == 0U || file_size > NVABI_FIRMWARE_MAX_BYTES ||
            file_size > UINT32_MAX || file_size > SIZE_MAX) {
            vfs_close(file);
            file = NULL;
            continue;
        }
        data = (uint8_t *)kmalloc((size_t)file_size, 0U);
        if (data == NULL) {
            vfs_close(file);
            file = NULL;
            continue;
        }
        bytes = 0U;
        status = vfs_read_kernel(file, data, (size_t)file_size, &bytes);
        vfs_close(file);
        file = NULL;
        if (status != K_OK || bytes != file_size) {
            kfree(data);
            data = NULL;
            continue;
        }
        break;
    }

    if (data == NULL) {
        nvabi_firmware_fail(name, type, family);
        return NULL;
    }

    sha256_compute(data, (UINTN)file_size, digest);
    nvabi_firmware_hash_text(digest, digest_text);
    if (!nvabi_firmware_hash_matches(digest, expected_hash)) {
        liteos_serial_printf_serial_only(
            "LITEOS_NVIDIA_FIRMWARE_HASH_FAIL path=%s size=%u sha256=%s expectation=595.84\r\n",
            resolved, (unsigned)file_size, digest_text);
        kfree(data);
        return NULL;
    }

    handle = (nvabi_firmware_handle_t *)kzalloc(sizeof(*handle), 0U);
    if (handle == NULL) {
        kfree(data);
        return NULL;
    }
    handle->magic = NVABI_FIRMWARE_MAGIC;
    handle->size = (uint32_t)file_size;
    handle->type = type;
    handle->family = family;
    handle->data = data;
    nvabi_memcpy(handle->path, resolved, nvabi_strlen(resolved) + 1U);

    *fw_buf = handle->data;
    *fw_size = handle->size;
    liteos_serial_printf_serial_only(
        "LITEOS_NVIDIA_FIRMWARE_RESOLVED path=%s size=%u sha256=%s expectation=595.84 hash_ok=1\r\n",
        handle->path, (unsigned)handle->size, digest_text);
    return handle;
}

void NV_API_CALL nv_put_firmware(const void *fw_handle) {
    nvabi_firmware_handle_t *handle = (nvabi_firmware_handle_t *)fw_handle;
    if (handle == NULL || handle->magic != NVABI_FIRMWARE_MAGIC) {
        liteos_serial_write_serial_only(
            "LITEOS_NVIDIA_FIRMWARE_RELEASE_FAIL\r\n");
        return;
    }
    handle->magic = 0U;
    if (handle->data != NULL) kfree(handle->data);
    handle->data = NULL;
    kfree(handle);
}
