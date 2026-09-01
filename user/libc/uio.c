#include "liteos/libc.h"

#include <limits.h>
#include <sys/uio.h>

static int validate_vectors(const struct iovec *vectors, int count,
                            size_t *total) {
    size_t length = 0U;
    if (count < 0 || count > IOV_MAX || (count != 0 && vectors == 0)) {
        errno = EINVAL;
        return -1;
    }
    for (int index = 0; index < count; ++index) {
        if (vectors[index].iov_base == 0 && vectors[index].iov_len != 0U) {
            errno = EFAULT;
            return -1;
        }
        if (vectors[index].iov_len > SIZE_MAX - length) {
            errno = EOVERFLOW;
            return -1;
        }
        length += vectors[index].iov_len;
        if (length > (size_t)INT64_MAX) {
            errno = EOVERFLOW;
            return -1;
        }
    }
    if (total != 0) *total = length;
    return 0;
}

static ssize_t transfer_vectors(int descriptor, const struct iovec *vectors,
                                int count, off_t offset, bool positional,
                                bool writing) {
    size_t total;
    size_t completed = 0U;
    if (validate_vectors(vectors, count, &total) < 0) return -1;
    if (positional && offset < 0) {
        errno = EINVAL;
        return -1;
    }
    for (int index = 0; index < count; ++index) {
        ssize_t result;
        if (vectors[index].iov_len == 0U) continue;
        if (writing) {
            result = positional ?
                pwrite(descriptor, vectors[index].iov_base,
                       vectors[index].iov_len, offset) :
                write(descriptor, vectors[index].iov_base,
                      vectors[index].iov_len);
        } else {
            result = positional ?
                pread(descriptor, vectors[index].iov_base,
                      vectors[index].iov_len, offset) :
                read(descriptor, vectors[index].iov_base,
                     vectors[index].iov_len);
        }
        if (result < 0) return completed != 0U ? (ssize_t)completed : -1;
        if (result == 0) break;
        if ((size_t)result > total - completed) {
            errno = EOVERFLOW;
            return -1;
        }
        completed += (size_t)result;
        if (positional) {
            if (offset > INT64_MAX - (off_t)result) {
                errno = EOVERFLOW;
                return -1;
            }
            offset += (off_t)result;
        }
        if ((size_t)result < vectors[index].iov_len) break;
    }
    return (ssize_t)completed;
}

ssize_t readv(int descriptor, const struct iovec *vectors, int count) {
    return transfer_vectors(descriptor, vectors, count, 0, false, false);
}

ssize_t writev(int descriptor, const struct iovec *vectors, int count) {
    return transfer_vectors(descriptor, vectors, count, 0, false, true);
}

ssize_t preadv(int descriptor, const struct iovec *vectors, int count,
               off_t offset) {
    return transfer_vectors(descriptor, vectors, count, offset, true, false);
}

ssize_t pwritev(int descriptor, const struct iovec *vectors, int count,
                off_t offset) {
    return transfer_vectors(descriptor, vectors, count, offset, true, true);
}
