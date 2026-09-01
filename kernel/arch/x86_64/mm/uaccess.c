#include <arch/x86_64/uaccess.h>

#define X86_USER_MIN_ADDRESS 0x0000000000010000ULL
#define X86_USER_END         0x0000800000000000ULL

typedef struct {
    uintptr_t fault;
    uintptr_t fixup;
} uaccess_fixup_t;

extern const uaccess_fixup_t liteos_uaccess_table_start[];
extern const uaccess_fixup_t liteos_uaccess_table_end[];
extern kstatus_t x86_copy_from_user_raw(void *destination, const void *source, size_t size);
extern kstatus_t x86_copy_to_user_raw(void *destination, const void *source, size_t size);
extern kstatus_t x86_get_user_u32_raw(uint32_t *value, const uint32_t *source);
extern kstatus_t x86_put_user_u32_raw(uint32_t *destination, uint32_t value);

uint8_t x86_uaccess_smap_enabled;

void x86_uaccess_init(bool smap_enabled) {
    x86_uaccess_smap_enabled = smap_enabled ? 1U : 0U;
}

bool x86_user_range_valid(const void __user *pointer, size_t size) {
    uintptr_t start = (uintptr_t)pointer;
    if (size == 0) return true;
    if (start < X86_USER_MIN_ADDRESS || start >= X86_USER_END) return false;
    return size <= X86_USER_END - start;
}

kstatus_t copy_from_user(void *destination, const void __user *source, size_t size) {
    if (size == 0) return K_OK;
    if (destination == 0 || !x86_user_range_valid(source, size)) return K_EINVAL;
    return x86_copy_from_user_raw(destination, source, size);
}

kstatus_t copy_to_user(void __user *destination, const void *source, size_t size) {
    if (size == 0) return K_OK;
    if (source == 0 || !x86_user_range_valid(destination, size)) return K_EINVAL;
    return x86_copy_to_user_raw(destination, source, size);
}

kstatus_t get_user_u32(uint32_t *value, const uint32_t __user *source) {
    if (value == 0 || ((uintptr_t)source & 3U) != 0 ||
        !x86_user_range_valid(source, sizeof(*source))) return K_EINVAL;
    return x86_get_user_u32_raw(value, source);
}

kstatus_t put_user_u32(uint32_t __user *destination, uint32_t value) {
    if (((uintptr_t)destination & 3U) != 0 ||
        !x86_user_range_valid(destination, sizeof(*destination))) return K_EINVAL;
    return x86_put_user_u32_raw(destination, value);
}

bool x86_uaccess_fixup(arch_trap_frame_t *frame) {
    if (frame == 0 || frame->vector != 14U) return false;
    for (const uaccess_fixup_t *entry = liteos_uaccess_table_start;
         entry < liteos_uaccess_table_end; ++entry) {
        if (frame->rip == entry->fault) {
            frame->rip = entry->fixup;
            return true;
        }
    }
    return false;
}

bool x86_uaccess_fault_site(const arch_trap_frame_t *frame) {
    if (frame == 0 || frame->vector != 14U) return false;
    for (const uaccess_fixup_t *entry = liteos_uaccess_table_start;
         entry < liteos_uaccess_table_end; ++entry) {
        if (frame->rip == entry->fault) return true;
    }
    return false;
}
