#include <kernel/credential.h>
#include <kernel/kmem.h>
#include <kernel/spinlock.h>

typedef struct credential_user {
    uint32_t uid;
    uint32_t gid;
    uint64_t groups;
    uint64_t privileges;
    capability_mask_t capabilities;
    uint8_t digest[CREDENTIAL_DIGEST_SIZE];
    uint32_t failed_attempts;
    bool enabled;
    bool used;
} credential_user_t;

static struct {
    spinlock_t lock;
    atomic_uint init_state;
    credential_user_t users[CREDENTIAL_MAX_USERS];
} g_credentials;

static void credential_lock(void) {
    while (atomic_exchange_explicit(&g_credentials.lock.state, 1U,
                                    memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
}

static void credential_unlock(void) {
    atomic_store_explicit(&g_credentials.lock.state, 0U, memory_order_release);
}

bool credential_manager_init(void) {
    unsigned expected = 0U;
    if (atomic_compare_exchange_strong_explicit(&g_credentials.init_state, &expected, 1U,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        atomic_init(&g_credentials.lock.state, 0U);
        for (uint32_t i = 0; i < CREDENTIAL_MAX_USERS; ++i) {
            g_credentials.users[i].used = false;
            g_credentials.users[i].enabled = false;
        }
        atomic_store_explicit(&g_credentials.init_state, 2U, memory_order_release);
        return true;
    }
    while (atomic_load_explicit(&g_credentials.init_state, memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
    return true;
}

static credential_user_t *credential_find_locked(uint32_t uid) {
    for (uint32_t i = 0; i < CREDENTIAL_MAX_USERS; ++i) {
        if (g_credentials.users[i].used && g_credentials.users[i].uid == uid) {
            return &g_credentials.users[i];
        }
    }
    return 0;
}

static bool credential_digest_equal(const uint8_t *left, const uint8_t *right) {
    uint8_t difference = 0U;
    for (uint32_t i = 0; i < CREDENTIAL_DIGEST_SIZE; ++i) {
        difference |= (uint8_t)(left[i] ^ right[i]);
    }
    return difference == 0U;
}

kstatus_t credential_add_user(uint32_t uid, uint32_t gid, uint64_t groups,
                              uint64_t privileges, capability_mask_t capabilities,
                              const uint8_t digest[CREDENTIAL_DIGEST_SIZE]) {
    credential_user_t *free_user = 0;
    if (digest == 0 || !credential_manager_init()) return K_EINVAL;
    credential_lock();
    if (credential_find_locked(uid) != 0) {
        credential_unlock();
        return K_EBUSY;
    }
    for (uint32_t i = 0; i < CREDENTIAL_MAX_USERS; ++i) {
        if (!g_credentials.users[i].used) {
            free_user = &g_credentials.users[i];
            break;
        }
    }
    if (free_user == 0) {
        credential_unlock();
        return K_ENOMEM;
    }
    free_user->uid = uid;
    free_user->gid = gid;
    free_user->groups = groups;
    free_user->privileges = privileges;
    free_user->capabilities = capabilities;
    for (uint32_t i = 0; i < CREDENTIAL_DIGEST_SIZE; ++i) {
        free_user->digest[i] = digest[i];
    }
    free_user->failed_attempts = 0U;
    free_user->enabled = true;
    free_user->used = true;
    credential_unlock();
    return K_OK;
}

kstatus_t credential_remove_user(uint32_t uid) {
    if (!credential_manager_init()) return K_EIO;
    credential_lock();
    credential_user_t *user = credential_find_locked(uid);
    if (user == 0) {
        credential_unlock();
        return K_ENOENT;
    }
    for (uint32_t i = 0; i < sizeof(*user); ++i) ((uint8_t *)user)[i] = 0U;
    credential_unlock();
    return K_OK;
}

kstatus_t credential_set_enabled(uint32_t uid, bool enabled) {
    if (!credential_manager_init()) return K_EIO;
    credential_lock();
    credential_user_t *user = credential_find_locked(uid);
    if (user == 0) {
        credential_unlock();
        return K_ENOENT;
    }
    user->enabled = enabled;
    if (enabled) user->failed_attempts = 0U;
    credential_unlock();
    return K_OK;
}

kstatus_t credential_authenticate(uint32_t uid,
                                  const uint8_t digest[CREDENTIAL_DIGEST_SIZE],
                                  security_token_t **out) {
    uint32_t gid;
    uint64_t groups;
    uint64_t privileges;
    capability_mask_t capabilities;
    if (digest == 0 || out == 0 || !credential_manager_init()) return K_EINVAL;
    *out = 0;
    credential_lock();
    credential_user_t *user = credential_find_locked(uid);
    if (user == 0 || !user->enabled || user->failed_attempts >= CREDENTIAL_MAX_FAILURES) {
        credential_unlock();
        return K_EACCES;
    }
    if (!credential_digest_equal(user->digest, digest)) {
        ++user->failed_attempts;
        if (user->failed_attempts >= CREDENTIAL_MAX_FAILURES) user->enabled = false;
        credential_unlock();
        return K_EACCES;
    }
    user->failed_attempts = 0U;
    gid = user->gid;
    groups = user->groups;
    privileges = user->privileges;
    capabilities = user->capabilities;
    credential_unlock();
    return security_token_create(uid, gid, groups, privileges, capabilities, out);
}

kstatus_t credential_login(uint32_t uid,
                           const uint8_t digest[CREDENTIAL_DIGEST_SIZE],
                           session_t **out) {
    security_token_t *token = 0;
    kstatus_t status;
    if (out == 0) return K_EINVAL;
    *out = 0;
    status = credential_authenticate(uid, digest, &token);
    if (status != K_OK) return status;
    status = session_create(token, 0, out);
    object_put(token);
    return status;
}

kstatus_t credential_session_close(session_t *session) {
    if (session == 0 || session->object.type != KOBJECT_TYPE_SESSION) return K_EINVAL;
    if (session->state == RESOURCE_STATE_CLOSED) return K_OK;
    if (session->state != RESOURCE_STATE_ACTIVE) return K_EBUSY;
    session->state = RESOURCE_STATE_CLOSED;
    return K_OK;
}

bool credential_core_self_test(void) {
    uint8_t good[CREDENTIAL_DIGEST_SIZE];
    uint8_t bad[CREDENTIAL_DIGEST_SIZE];
    security_token_t *token = 0;
    session_t *session = 0;
    bool success = true;
    for (uint32_t i = 0; i < CREDENTIAL_DIGEST_SIZE; ++i) {
        good[i] = (uint8_t)(0x30U + i);
        bad[i] = 0U;
    }
    if (credential_add_user(1001U, 100U, 1ULL << 3, 0x10U, 0U, good) != K_OK ||
        credential_authenticate(1001U, bad, &token) != K_EACCES ||
        credential_authenticate(1001U, good, &token) != K_OK || token == 0 ||
        token->uid != 1001U || token->gid != 100U) success = false;
    if (token != 0) {
        object_put(token);
        token = 0;
    }
    if (credential_login(1001U, good, &session) != K_OK || session == 0 ||
        session->token == 0 || session->token->uid != 1001U ||
        credential_session_close(session) != K_OK ||
        session->state != RESOURCE_STATE_CLOSED) success = false;
    if (session != 0) {
        object_put(session);
        session = 0;
    }
    if (credential_add_user(1002U, 100U, 0U, 0U, 0U, good) != K_OK) success = false;
    for (uint32_t i = 0; i < CREDENTIAL_MAX_FAILURES; ++i) {
        if (credential_authenticate(1002U, bad, &token) != K_EACCES) success = false;
    }
    if (credential_authenticate(1002U, good, &token) != K_EACCES ||
        credential_set_enabled(1002U, true) != K_OK ||
        credential_authenticate(1002U, good, &token) != K_OK) success = false;
    if (token != 0) object_put(token);
    if (credential_remove_user(1001U) != K_OK || credential_remove_user(1002U) != K_OK) {
        success = false;
    }
    return success;
}
