#pragma once

#include "base.h"
#include "resource.h"
#include "security.h"

#define CREDENTIAL_DIGEST_SIZE 32U
#define CREDENTIAL_MAX_USERS 64U
#define CREDENTIAL_MAX_FAILURES 5U

bool credential_manager_init(void);
kstatus_t credential_add_user(uint32_t uid, uint32_t gid, uint64_t groups,
                              uint64_t privileges, capability_mask_t capabilities,
                              const uint8_t digest[CREDENTIAL_DIGEST_SIZE]);
kstatus_t credential_remove_user(uint32_t uid);
kstatus_t credential_set_enabled(uint32_t uid, bool enabled);
kstatus_t credential_authenticate(uint32_t uid,
                                  const uint8_t digest[CREDENTIAL_DIGEST_SIZE],
                                  security_token_t **out);
kstatus_t credential_login(uint32_t uid,
                           const uint8_t digest[CREDENTIAL_DIGEST_SIZE],
                           session_t **out);
kstatus_t credential_session_close(session_t *session);
bool credential_core_self_test(void);
