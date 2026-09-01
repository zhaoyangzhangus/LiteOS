/* REFACTOR_SYSCALL_DEBUG_OWNER: bounded user diagnostics stay serial-only. */

#include <arch/x86_64/uaccess.h>
#include <kernel/console.h>

#include "internal.h"

#define DEBUG_WRITE_MAX_BYTES 240U

int64_t syscall_debug_write(uint64_t buffer, uint64_t length,
                            uint64_t unused2, uint64_t unused3,
                            uint64_t unused4, uint64_t unused5) {
    char text[DEBUG_WRITE_MAX_BYTES + 1U];
    kstatus_t status;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    if (length > DEBUG_WRITE_MAX_BYTES) return K_EINVAL;
    if (length == 0U) return 0;
    status = copy_from_user(text, (const void __user *)(uintptr_t)buffer,
                            (size_t)length);
    if (status != K_OK) return status;
    text[length] = '\0';
    liteos_serial_write_serial_only(text);
    return (int64_t)length;
}
