#include <kernel/update_boot.h>

static CHAR16 g_liteos_update_state_name[] = {
    'L','i','t','e','O','S','U','p','d','a','t','e','S','t','a','t','e',0
};
static const EFI_GUID g_liteos_update_vendor_guid = LITEOS_UPDATE_VENDOR_GUID_INITIALIZER;

static UINT32 update_boot_checksum(const LITEOS_UPDATE_STATE *state) {
    const UINT8 *bytes = (const UINT8 *)state;
    UINT32 hash = 2166136261U;
    UINTN length = sizeof(*state) - sizeof(UINT32) * 2U;
    for (UINTN i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= 16777619U;
    }
    return hash;
}

static BOOLEAN update_boot_state_valid(const LITEOS_UPDATE_STATE *state) {
    if (state == 0 || state->Magic != LITEOS_UPDATE_STATE_MAGIC ||
        state->Version != LITEOS_UPDATE_STATE_VERSION ||
        (state->ActiveSlot != LITEOS_UPDATE_SLOT_A && state->ActiveSlot != LITEOS_UPDATE_SLOT_B) ||
        (state->PendingSlot != LITEOS_UPDATE_SLOT_A && state->PendingSlot != LITEOS_UPDATE_SLOT_B) ||
        state->PendingSlot == state->ActiveSlot ||
        state->BootAttempts == 0U ||
        state->BootAttempts > LITEOS_UPDATE_MAX_BOOT_ATTEMPTS || state->SafeMode != 0U ||
        state->Reserved != 0U ||
        ((state->ActiveVersion == 0U) != (state->Generation == 0U)) ||
        state->PendingVersion == 0U || state->PendingGeneration == 0U ||
        state->PendingGeneration <= state->Generation ||
        state->Checksum != update_boot_checksum(state)) return 0;
    return 1;
}

bool kernel_update_commit_boot(const LITEOS_BOOT_INFO *info) {
    EFI_RUNTIME_SERVICES *runtime;
    LITEOS_UPDATE_STATE state;
    UINT32 attributes = 0;
    UINTN size = sizeof(state);
    EFI_STATUS status;

    if (info == 0 || (info->Flags & LITEOS_BOOTINFO_UPDATE_PENDING) == 0) return true;
    if ((info->UpdateBootSlot != LITEOS_UPDATE_SLOT_A &&
         info->UpdateBootSlot != LITEOS_UPDATE_SLOT_B) ||
        info->RuntimeServices == 0) return false;
    runtime = (EFI_RUNTIME_SERVICES *)(uintptr_t)info->RuntimeServices;
    if (runtime->GetVariable == 0 || runtime->SetVariable == 0) return false;
    status = runtime->GetVariable(g_liteos_update_state_name,
                                  (EFI_GUID *)&g_liteos_update_vendor_guid,
                                  &attributes, &size, &state);
    if (EFI_ERROR(status) || size != sizeof(state) ||
        !update_boot_state_valid(&state) ||
        state.PendingSlot != info->UpdateBootSlot ||
        state.BootAttempts != info->UpdateBootAttempts) return false;

    state.ActiveSlot = state.PendingSlot;
    state.ActiveVersion = state.PendingVersion;
    state.Generation = state.PendingGeneration;
    state.PendingSlot = LITEOS_UPDATE_SLOT_NONE;
    state.PendingVersion = 0U;
    state.PendingGeneration = 0U;
    state.BootAttempts = 0U;
    state.SafeMode = 0U;
    state.Checksum = update_boot_checksum(&state);
    status = runtime->SetVariable(g_liteos_update_state_name,
                                  (EFI_GUID *)&g_liteos_update_vendor_guid,
                                  EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
                                      EFI_VARIABLE_RUNTIME_ACCESS,
                                  sizeof(state), &state);
    return !EFI_ERROR(status);
}
