#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>

#define __user
#define __iomem
#define __percpu
#define __must_check
#define __noreturn _Noreturn
#define __packed __attribute__((packed))
#define __aligned(x) __attribute__((aligned(x)))

#define PAGE_SHIFT 12u
#define PAGE_SIZE  (1ull << PAGE_SHIFT)
#define CACHELINE_SIZE 64u
#define MAX_CPUS 256u

typedef int64_t  kstatus_t;
typedef uint64_t pfn_t;
typedef uintptr_t vaddr_t;

typedef struct { uint64_t value; } paddr_t;
typedef struct { uint64_t value; } iova_t;
typedef struct { uint64_t value; } gpu_va_t;

static inline paddr_t paddr_make(uint64_t v) { return (paddr_t){v}; }
static inline iova_t  iova_make(uint64_t v)  { return (iova_t){v}; }
static inline gpu_va_t gpu_va_make(uint64_t v) { return (gpu_va_t){v}; }

enum {
    K_OK       = 0,
    K_EPERM    = -1,
    K_ENOENT   = -2,
    K_EIO      = -5,
    K_EAGAIN   = -11,
    K_ENOMEM   = -12,
    K_EACCES   = -13,
    K_EBUSY    = -16,
    K_EINVAL   = -22,
    K_ENOSYS   = -38,
    K_ETIMEDOUT= -110,
    K_ECANCELED= -125,
    K_EDEVREMOVED = -2001,
    K_EDEVLOST    = -2002,
};
