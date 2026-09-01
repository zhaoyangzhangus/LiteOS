#pragma once

#include <stddef.h>
#include <sys/types.h>

struct iovec {
    void *iov_base;
    size_t iov_len;
};

#define IOV_MAX 1024

ssize_t readv(int descriptor, const struct iovec *vectors, int count);
ssize_t writev(int descriptor, const struct iovec *vectors, int count);
ssize_t preadv(int descriptor, const struct iovec *vectors, int count,
               off_t offset);
ssize_t pwritev(int descriptor, const struct iovec *vectors, int count,
                off_t offset);
