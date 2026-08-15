#ifndef LITEOS_KERNEL_PACKAGE_H
#define LITEOS_KERNEL_PACKAGE_H

#include <kernel/base.h>
#include <kernel/spinlock.h>

#define PACKAGE_ID_SIZE 16U
#define PACKAGE_DIGEST_SIZE 32U
#define PACKAGE_NAME_CAP 48U
#define PACKAGE_REGISTRY_CAPACITY 64U

typedef struct package_identity {
    uint8_t package_id[PACKAGE_ID_SIZE];
    uint64_t version;
    uint64_t generation;
    uint32_t owner_uid;
    uint32_t owner_gid;
    uint32_t flags;
    uint32_t reserved;
    uint8_t payload_digest[PACKAGE_DIGEST_SIZE];
    char name[PACKAGE_NAME_CAP];
} package_identity_t;

typedef struct package_registry_entry {
    bool registered;
    package_identity_t identity;
} package_registry_entry_t;

typedef struct package_registry {
    spinlock_t lock;
    atomic_uint init_state;
    package_registry_entry_t entries[PACKAGE_REGISTRY_CAPACITY];
} package_registry_t;

bool package_identity_valid(const package_identity_t *identity);
bool package_registry_init(package_registry_t *registry);
kstatus_t package_register(package_registry_t *registry,
                           const package_identity_t *identity);
kstatus_t package_lookup(package_registry_t *registry,
                         const uint8_t package_id[PACKAGE_ID_SIZE],
                         package_identity_t *identity);
kstatus_t package_unregister(package_registry_t *registry,
                             const uint8_t package_id[PACKAGE_ID_SIZE]);
bool package_core_self_test(void);

#endif
