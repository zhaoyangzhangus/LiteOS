#pragma once

/*
 * NVIDIA 595.84 -> LiteOS ABI boundary.
 *
 * The declarations below intentionally come from the immutable NVIDIA
 * headers.  This layer is the only place where vendor symbols are translated
 * to LiteOS primitives.  The port itself remains vendor-name free and does
 * not implement VRAM PMA, GPU VA, GMMU page tables, or GSP RPC.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

#include "nv_port.h"
#include "nvstatus.h"
#include "os-interface.h"
#include "nv.h"
#include "nv-caps.h"
#include "nv-reg.h"
#include <arch/x86_64/cpu.h>
#include <kernel/console.h>
#include <kernel/process.h>
#include <kernel/sched.h>

#define NVABI_PAGE_SIZE 4096ULL
#define NVABI_PAGE_MASK (NVABI_PAGE_SIZE - 1ULL)

/* Private descriptor for system-memory allocations returned through the
 * NVIDIA nv_alloc_pages ABI.  It carries only LiteOS CPU/system pages; it is
 * deliberately not a VRAM, GPU-VA, GMMU, or GSP object. */
typedef struct nvabi_page_allocation {
    uint32_t magic;
    uint32_t page_count;
    uint32_t cache_type;
    NvBool contiguous;
    nv_port_mem_t memory;
} nvabi_page_allocation_t;

#define NVABI_PAGE_ALLOCATION_MAGIC 0x4E564D45U

static inline NV_STATUS nvabi_status_from_kstatus(kstatus_t status) {
    if (status == K_OK) return NV_OK;
    if (status == K_ENOMEM) return NV_ERR_NO_MEMORY;
    if (status == K_EINVAL || status == K_EOVERFLOW) {
        return NV_ERR_INVALID_ARGUMENT;
    }
    if (status == K_EACCES || status == K_EPERM) {
        return NV_ERR_INSUFFICIENT_PERMISSIONS;
    }
    if (status == K_ETIMEDOUT) return NV_ERR_TIMEOUT;
    if (status == K_ENOSYS) {
        return NV_ERR_NOT_SUPPORTED;
    }
    return NV_ERR_GENERIC;
}

static inline NvBool nvabi_bool_from_kstatus(kstatus_t status) {
    return status == K_OK ? NV_TRUE : NV_FALSE;
}

void nvabi_log_unsupported(const char *function, const char *caller_stage);
void nvabi_log_invalid(const char *function, const char *caller_stage);

static inline NV_STATUS nvabi_unsupported_status(const char *function,
                                                  const char *caller_stage) {
    nvabi_log_unsupported(function, caller_stage);
    return NV_ERR_NOT_SUPPORTED;
}

static inline NV_STATUS nvabi_invalid_status(const char *function,
                                              const char *caller_stage) {
    nvabi_log_invalid(function, caller_stage);
    return NV_ERR_INVALID_ARGUMENT;
}

static inline void *nvabi_zero_pointer(const char *function,
                                       const char *caller_stage) {
    nvabi_log_unsupported(function, caller_stage);
    return NULL;
}

static inline NvBool nvabi_unsupported_bool(const char *function,
                                            const char *caller_stage) {
    nvabi_log_unsupported(function, caller_stage);
    return NV_FALSE;
}

static inline NvS32 nvabi_unsupported_s32(const char *function,
                                          const char *caller_stage) {
    nvabi_log_unsupported(function, caller_stage);
    return (NvS32)K_ENOSYS;
}

static inline void nvabi_unsupported_void(const char *function,
                                           const char *caller_stage) {
    nvabi_log_unsupported(function, caller_stage);
}

/* Small freestanding helpers used by ABI implementations. */
static inline size_t nvabi_strlen(const char *string) {
    size_t length = 0U;
    if (string == NULL) return 0U;
    while (string[length] != '\0') ++length;
    return length;
}

static inline void *nvabi_memcpy(void *destination, const void *source,
                                 size_t length) {
    uint8_t *dst = (uint8_t *)destination;
    const uint8_t *src = (const uint8_t *)source;
    if (destination == NULL || source == NULL) return NULL;
    for (size_t index = 0U; index < length; ++index) dst[index] = src[index];
    return destination;
}

static inline void *nvabi_memset(void *destination, int value, size_t length) {
    uint8_t *dst = (uint8_t *)destination;
    if (destination == NULL) return NULL;
    for (size_t index = 0U; index < length; ++index) {
        dst[index] = (uint8_t)value;
    }
    return destination;
}

static inline int nvabi_memcmp(const void *left, const void *right,
                               size_t length) {
    const uint8_t *a = (const uint8_t *)left;
    const uint8_t *b = (const uint8_t *)right;
    if (left == NULL || right == NULL) return left == right ? 0 : 1;
    for (size_t index = 0U; index < length; ++index) {
        if (a[index] != b[index]) return a[index] < b[index] ? -1 : 1;
    }
    return 0;
}
