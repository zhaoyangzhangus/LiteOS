#ifndef LITEOS_KERNEL_FIRMWARE_H
#define LITEOS_KERNEL_FIRMWARE_H

#include <kernel/base.h>
#include <kernel/sha256.h>
#include <kernel/spinlock.h>

#define FIRMWARE_NAME_CAP 96U
#define FIRMWARE_PACKAGE_ID_SIZE 16U

/*
 * Firmware Manager 不直接知道文件系统或具体总线。
 * 驱动只提交逻辑名称，供应者负责从已签名的包/文件系统中解析实际数据。
 */
typedef struct firmware_blob {
    const void *data;
    size_t size;
    uint64_t version;
    uint64_t generation;
    uint8_t package_id[FIRMWARE_PACKAGE_ID_SIZE];
    uint8_t digest[KERNEL_SHA256_DIGEST_SIZE];
} firmware_blob_t;

typedef kstatus_t (*firmware_resolve_fn)(const char *logical_name,
                                         uint64_t minimum_version,
                                         firmware_blob_t *out,
                                         void *context);
typedef void (*firmware_release_fn)(firmware_blob_t *blob, void *context);

typedef struct firmware_manager {
    spinlock_t lock;
    atomic_uint init_state;
    firmware_resolve_fn resolve;
    firmware_release_fn release;
    void *context;
} firmware_manager_t;

/* 初始化一次；resolve/release 的生命周期必须覆盖 manager 的使用期。 */
kstatus_t firmware_manager_init(firmware_manager_t *manager,
                                firmware_resolve_fn resolve,
                                firmware_release_fn release,
                                void *context);

/* 解析并校验固件；成功后调用者必须调用 firmware_release。 */
kstatus_t firmware_request(firmware_manager_t *manager,
                           const char *logical_name,
                           uint64_t minimum_version,
                           firmware_blob_t *out);
void firmware_release(firmware_manager_t *manager, firmware_blob_t *blob);

bool firmware_logical_name_valid(const char *logical_name);
bool firmware_core_self_test(void);

#endif
