#pragma once

#include <kernel/base.h>
#include <kernel/sha256.h>

#define UPDATE_MANIFEST_VERSION 1U
#define UPDATE_PACKAGE_ID_SIZE 16U
#define UPDATE_SIGNATURE_SIZE 256U /* RSA-2048 PKCS#1 v1.5 */
#define UPDATE_MAX_BOOT_ATTEMPTS 3U
#define UPDATE_STATE_MAGIC 0x4C555053U /* "LUPS" */

typedef enum update_slot {
    UPDATE_SLOT_NONE = 0,
    UPDATE_SLOT_A = 1,
    UPDATE_SLOT_B = 2,
} update_slot_t;

typedef struct update_manifest {
    uint32_t format_version;
    uint32_t slot;
    uint64_t version;
    uint64_t generation;
    uint64_t image_size;
    uint8_t package_id[UPDATE_PACKAGE_ID_SIZE];
    uint8_t digest[KERNEL_SHA256_DIGEST_SIZE];
    uint8_t signature[UPDATE_SIGNATURE_SIZE];
} update_manifest_t;

typedef struct update_state {
    uint32_t magic;
    uint32_t version;
    uint32_t active_slot;
    uint32_t pending_slot;
    uint32_t boot_attempts;
    uint32_t safe_mode;
    uint64_t confirmed_version;
    uint64_t pending_version;
    uint64_t generation;
    uint64_t pending_generation;
} update_state_t;

typedef bool (*update_signature_verify_fn)(const update_manifest_t *manifest,
                                          const void *image, size_t image_size,
                                          void *context);
typedef kstatus_t (*update_state_load_fn)(update_state_t *state, void *context);
typedef kstatus_t (*update_state_store_fn)(const update_state_t *state, void *context);

typedef struct update_manager update_manager_t;

kstatus_t update_manager_create(update_signature_verify_fn verify,
                                update_state_load_fn load,
                                update_state_store_fn store, void *context,
                                update_manager_t **out);
void update_manager_destroy(update_manager_t *manager);
kstatus_t update_stage(update_manager_t *manager, const update_manifest_t *manifest,
                       const void *image, size_t image_size);
kstatus_t update_select_boot(update_manager_t *manager, update_slot_t *slot);
kstatus_t update_mark_boot_success(update_manager_t *manager);
kstatus_t update_mark_boot_failure(update_manager_t *manager);
kstatus_t update_enter_safe_mode(update_manager_t *manager);
kstatus_t update_clear_safe_mode(update_manager_t *manager);
kstatus_t update_get_state(update_manager_t *manager, update_state_t *state);
bool update_core_self_test(void);
