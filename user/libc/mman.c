#include "liteos/libc.h"

#include <sys/mman.h>

#define LIBC_PAGE_SIZE 4096U

static int invalid_mapping(void) {
    errno = EINVAL;
    return -1;
}

static int round_mapping_length(size_t length, size_t *rounded) {
    size_t remainder;
    if (rounded == 0 || length == 0U ||
        length > SIZE_MAX - (LIBC_PAGE_SIZE - 1U)) {
        return invalid_mapping();
    }
    remainder = length & (LIBC_PAGE_SIZE - 1U);
    *rounded = remainder == 0U ? length :
               length + LIBC_PAGE_SIZE - remainder;
    return 0;
}

static int translate_protection(int protection, uint32_t *result) {
    uint32_t translated = 0U;
    if (result == 0 ||
        (protection & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) != 0) {
        return invalid_mapping();
    }
    if ((protection & PROT_READ) != 0) translated |= OS_VM_READ;
    if ((protection & PROT_WRITE) != 0) translated |= OS_VM_WRITE;
    if ((protection & PROT_EXEC) != 0) translated |= OS_VM_EXEC;
    *result = translated;
    return 0;
}

void *mmap(void *address, size_t length, int protection, int flags,
           int descriptor, off_t offset) {
    const int supported_flags = MAP_SHARED | MAP_PRIVATE | MAP_FIXED |
                                MAP_ANONYMOUS | MAP_STACK;
    os_vm_map_args_t request = {0};
    size_t mapped_length;
    uint32_t translated_protection;
    os_handle_t object = OS_INVALID_HANDLE;
    int64_t status;

    if (round_mapping_length(length, &mapped_length) < 0 ||
        translate_protection(protection, &translated_protection) < 0 ||
        (flags & ~supported_flags) != 0 ||
        ((flags & (MAP_SHARED | MAP_PRIVATE)) ==
         (MAP_SHARED | MAP_PRIVATE)) ||
        (flags & (MAP_SHARED | MAP_PRIVATE)) == 0 ||
        offset < 0 || ((uint64_t)offset & (LIBC_PAGE_SIZE - 1U)) != 0U) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    if ((flags & MAP_FIXED) != 0 &&
        ((uintptr_t)address & (LIBC_PAGE_SIZE - 1U)) != 0U) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    if ((flags & MAP_ANONYMOUS) == 0) {
        if (descriptor < 0) {
            errno = EBADF;
            return MAP_FAILED;
        }
        if (__libc_descriptor_is_socket(descriptor)) {
            errno = EBADF;
            return MAP_FAILED;
        }
        object = __libc_handle_for_descriptor(descriptor);
        if (object == OS_INVALID_HANDLE) return MAP_FAILED;
    } else if (offset != 0) {
        errno = EINVAL;
        return MAP_FAILED;
    }

    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.address = (uint64_t)(uintptr_t)address;
    request.length = mapped_length;
    request.offset = (uint64_t)offset;
    request.object = object;
    request.prot = translated_protection;
    request.flags = (flags & MAP_SHARED) != 0 ? OS_VM_SHARED : OS_VM_PRIVATE;
    if ((flags & MAP_FIXED) != 0) request.flags |= OS_VM_FIXED;
    if ((flags & MAP_STACK) != 0) request.flags |= OS_VM_STACK;
    status = liteos_syscall6(OS_SYS_VM_MAP,
                             (uint64_t)(uintptr_t)&request,
                             0U, 0U, 0U, 0U, 0U);
    if (status < 0 || request.address == 0U) {
        if (status < 0) (void)__libc_status_result(status);
        else errno = ENOMEM;
        return MAP_FAILED;
    }
    return (void *)(uintptr_t)request.address;
}

int munmap(void *address, size_t length) {
    size_t mapped_length;
    int64_t status;
    if (address == MAP_FAILED ||
        ((uintptr_t)address & (LIBC_PAGE_SIZE - 1U)) != 0U ||
        round_mapping_length(length, &mapped_length) < 0) {
        errno = EINVAL;
        return -1;
    }
    status = liteos_syscall6(OS_SYS_VM_UNMAP,
                             (uint64_t)(uintptr_t)address, mapped_length,
                             0U, 0U, 0U, 0U);
    return __libc_status_result(status);
}

int mprotect(void *address, size_t length, int protection) {
    size_t mapped_length;
    uint32_t translated_protection;
    int64_t status;
    if (address == MAP_FAILED ||
        ((uintptr_t)address & (LIBC_PAGE_SIZE - 1U)) != 0U ||
        round_mapping_length(length, &mapped_length) < 0 ||
        translate_protection(protection, &translated_protection) < 0) {
        errno = EINVAL;
        return -1;
    }
    status = liteos_syscall6(OS_SYS_VM_PROTECT,
                             (uint64_t)(uintptr_t)address, mapped_length,
                             translated_protection, 0U, 0U, 0U);
    return __libc_status_result(status);
}

int msync(void *address, size_t length, int flags) {
    size_t mapped_length;
    int sync_mode = flags & (MS_ASYNC | MS_SYNC);
    int64_t status;
    if (address == MAP_FAILED ||
        ((uintptr_t)address & (LIBC_PAGE_SIZE - 1U)) != 0U ||
        round_mapping_length(length, &mapped_length) < 0 ||
        (flags & ~(MS_ASYNC | MS_INVALIDATE | MS_SYNC)) != 0 ||
        (sync_mode == 0 || sync_mode == (MS_ASYNC | MS_SYNC))) {
        errno = EINVAL;
        return -1;
    }
    status = liteos_syscall6(OS_SYS_VM_SYNC,
                             (uint64_t)(uintptr_t)address, mapped_length,
                             (uint64_t)(uint32_t)flags, 0U, 0U, 0U);
    return __libc_status_result(status);
}

int madvise(void *address, size_t length, int advice) {
    size_t mapped_length;
    int64_t status;
    if (address == MAP_FAILED ||
        ((uintptr_t)address & (LIBC_PAGE_SIZE - 1U)) != 0U ||
        round_mapping_length(length, &mapped_length) < 0 ||
        (advice < MADV_NORMAL || advice > MADV_DONTNEED)) {
        errno = EINVAL;
        return -1;
    }
    status = liteos_syscall6(OS_SYS_VM_ADVISE,
                             (uint64_t)(uintptr_t)address, mapped_length,
                             (uint64_t)(uint32_t)advice, 0U, 0U, 0U);
    return __libc_status_result(status);
}

long sysconf(int name) {
    switch (name) {
    case _SC_PAGESIZE:
        return LIBC_PAGE_SIZE;
    case _SC_OPEN_MAX:
        return 256L;
    case _SC_CLK_TCK:
        return 100L;
    default:
        errno = EINVAL;
        return -1L;
    }
}

int getpagesize(void) {
    return LIBC_PAGE_SIZE;
}
