#ifndef LITEOS_UPDATE_STATE_H
#define LITEOS_UPDATE_STATE_H

#include <stdint.h>

/* UEFI 变量与 BootInfo 共用的 A/B 启动状态格式。 */
#define LITEOS_UPDATE_STATE_MAGIC 0x4C555053U /* "LUPS" */
#define LITEOS_UPDATE_STATE_VERSION 1U
#define LITEOS_UPDATE_MAX_BOOT_ATTEMPTS 3U

enum {
    LITEOS_UPDATE_SLOT_NONE = 0U,
    LITEOS_UPDATE_SLOT_A = 1U,
    LITEOS_UPDATE_SLOT_B = 2U,
};

typedef struct liteos_update_state {
    uint32_t Magic;
    uint32_t Version;
    uint32_t ActiveSlot;
    uint32_t PendingSlot;
    uint32_t BootAttempts;
    uint32_t SafeMode;
    uint64_t ActiveVersion;
    uint64_t PendingVersion;
    uint64_t Generation;
    uint64_t PendingGeneration;
    uint32_t Checksum;
    uint32_t Reserved;
} LITEOS_UPDATE_STATE;

#define LITEOS_UPDATE_VENDOR_GUID_INITIALIZER \
    {0x6D4C4954U, 0x454FU, 0x534FU, {0x4CU, 0x49U, 0x54U, 0x45U, 0x4FU, 0x53U, 0x01U, 0x01U}}

#endif
