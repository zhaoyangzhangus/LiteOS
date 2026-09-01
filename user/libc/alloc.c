#include "liteos/libc.h"

#define LIBC_PAGE_SIZE 4096U
#define LIBC_ALLOC_MAGIC 0x4C4954454F53414CULL
#define LIBC_DEFAULT_ALIGNMENT 16U

typedef struct libc_allocation {
    uint64_t magic;
    uint64_t mapping;
    uint64_t mapping_size;
    uint64_t requested_size;
} libc_allocation_t;

static bool is_power_of_two(size_t value) {
    return value != 0U && (value & (value - 1U)) == 0U;
}

static size_t page_align(size_t size) {
    size_t remainder;
    if (size == 0U || size > SIZE_MAX - (LIBC_PAGE_SIZE - 1U)) return 0U;
    remainder = size & (LIBC_PAGE_SIZE - 1U);
    return remainder == 0U ? size : size + LIBC_PAGE_SIZE - remainder;
}

static void *allocate_aligned(size_t size, size_t alignment) {
    os_vm_map_args_t request = {0};
    libc_allocation_t *allocation;
    uintptr_t base;
    uintptr_t user_address;
    size_t overhead;
    size_t total;
    int64_t status;

    if (size == 0U) size = 1U;
    if (alignment < LIBC_DEFAULT_ALIGNMENT) alignment = LIBC_DEFAULT_ALIGNMENT;
    if (!is_power_of_two(alignment) || alignment > (size_t)UINTPTR_MAX ||
        size > SIZE_MAX - sizeof(*allocation) ||
        size + sizeof(*allocation) > SIZE_MAX - (alignment - 1U)) {
        errno = EINVAL;
        return 0;
    }
    overhead = sizeof(*allocation) + alignment - 1U;
    total = page_align(size + overhead);
    if (total == 0U) {
        errno = EOVERFLOW;
        return 0;
    }
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.length = total;
    request.prot = OS_VM_READ | OS_VM_WRITE;
    request.flags = OS_VM_PRIVATE;
    status = liteos_syscall6(OS_SYS_VM_MAP,
                             (uint64_t)(uintptr_t)&request,
                             0U, 0U, 0U, 0U, 0U);
    if (status < 0 || request.address == 0U) {
        if (status < 0) errno = (int)(-status);
        else errno = ENOMEM;
        return 0;
    }
    base = (uintptr_t)request.address;
    if (base > UINTPTR_MAX - sizeof(*allocation) - (alignment - 1U)) {
        (void)liteos_syscall6(OS_SYS_VM_UNMAP, request.address, total,
                              0U, 0U, 0U, 0U);
        errno = EOVERFLOW;
        return 0;
    }
    user_address = (base + sizeof(*allocation) + alignment - 1U) &
                   ~((uintptr_t)alignment - 1U);
    allocation = (libc_allocation_t *)(user_address - sizeof(*allocation));
    allocation->magic = LIBC_ALLOC_MAGIC;
    allocation->mapping = request.address;
    allocation->mapping_size = total;
    allocation->requested_size = size;
    return (void *)user_address;
}

void *malloc(size_t size) {
    return allocate_aligned(size, LIBC_DEFAULT_ALIGNMENT);
}

void *calloc(size_t count, size_t size) {
    size_t total;
    void *pointer;
    if (count != 0U && size > SIZE_MAX / count) {
        errno = EOVERFLOW;
        return 0;
    }
    total = count * size;
    pointer = malloc(total);
    if (pointer != 0) memset(pointer, 0, total == 0U ? 1U : total);
    return pointer;
}

void *reallocarray(void *pointer, size_t count, size_t size) {
    if (count != 0U && size > SIZE_MAX / count) {
        errno = EOVERFLOW;
        return 0;
    }
    return realloc(pointer, count * size);
}

void *aligned_alloc(size_t alignment, size_t size) {
    if (!is_power_of_two(alignment) || alignment % sizeof(void *) != 0U ||
        size % alignment != 0U) {
        errno = EINVAL;
        return 0;
    }
    return allocate_aligned(size, alignment);
}

int posix_memalign(void **pointer, size_t alignment, size_t size) {
    void *result;
    if (pointer == 0) return EINVAL;
    if (!is_power_of_two(alignment) || alignment % sizeof(void *) != 0U) {
        return EINVAL;
    }
    result = allocate_aligned(size, alignment);
    if (result == 0) return errno != 0 ? errno : ENOMEM;
    *pointer = result;
    return 0;
}

void *memalign(size_t alignment, size_t size) {
    if (!is_power_of_two(alignment)) {
        errno = EINVAL;
        return 0;
    }
    return allocate_aligned(size, alignment);
}

void *valloc(size_t size) {
    return allocate_aligned(size, LIBC_PAGE_SIZE);
}

static libc_allocation_t *allocation_from_pointer(const void *pointer) {
    libc_allocation_t *allocation;
    if (pointer == 0) return 0;
    allocation = (libc_allocation_t *)((uintptr_t)pointer - sizeof(*allocation));
    if (allocation->magic != LIBC_ALLOC_MAGIC || allocation->mapping_size == 0U) {
        return 0;
    }
    return allocation;
}

size_t malloc_usable_size(const void *pointer) {
    libc_allocation_t *allocation = allocation_from_pointer(pointer);
    return allocation == 0 ? 0U : (size_t)allocation->requested_size;
}

void free(void *pointer) {
    libc_allocation_t *allocation = allocation_from_pointer(pointer);
    if (allocation == 0) return;
    allocation->magic = 0U;
    if (liteos_syscall6(OS_SYS_VM_UNMAP, allocation->mapping,
                        allocation->mapping_size, 0U, 0U, 0U, 0U) < 0) {
        errno = EIO;
    }
}

void *realloc(void *pointer, size_t size) {
    libc_allocation_t *allocation;
    size_t previous_size;
    void *replacement;

    if (pointer == 0) return malloc(size);
    if (size == 0U) {
        free(pointer);
        return 0;
    }
    allocation = allocation_from_pointer(pointer);
    if (allocation == 0) {
        errno = EINVAL;
        return 0;
    }
    previous_size = (size_t)allocation->requested_size;
    if (size <= previous_size) {
        allocation->requested_size = size;
        return pointer;
    }
    replacement = malloc(size);
    if (replacement == 0) return 0;
    memcpy(replacement, pointer, previous_size);
    free(pointer);
    return replacement;
}
