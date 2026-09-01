#include "liteos/libc.h"

#include <limits.h>
#include <poll.h>
#include <sys/select.h>

static int select_timeout_milliseconds(const struct timeval *timeout) {
    uint64_t seconds;
    uint64_t microseconds;
    uint64_t total;
    uint64_t milliseconds;
    if (timeout == 0) return -1;
    if (timeout->tv_sec < 0 || timeout->tv_usec < 0 || timeout->tv_usec >= 1000000L) {
        errno = EINVAL;
        return -2;
    }
    seconds = (uint64_t)timeout->tv_sec;
    microseconds = (uint64_t)timeout->tv_usec;
    if (seconds > (UINT64_MAX - microseconds) / UINT64_C(1000000)) {
        return INT_MAX;
    }
    total = seconds * UINT64_C(1000000) + microseconds;
    milliseconds = (total + 999U) / 1000U;
    return milliseconds > (uint64_t)INT_MAX ? INT_MAX : (int)milliseconds;
}

int select(int nfds, fd_set *readfds, fd_set *writefds,
           fd_set *exceptfds, struct timeval *timeout) {
    struct pollfd pollfds[FD_SETSIZE];
    int descriptors[FD_SETSIZE];
    int poll_timeout;
    nfds_t count = 0U;
    int result;
    int descriptor;
    int ready = 0;
    fd_set original_read;
    fd_set original_write;
    fd_set original_except;

    if (nfds < 0 || nfds > FD_SETSIZE) {
        errno = EINVAL;
        return -1;
    }
    poll_timeout = select_timeout_milliseconds(timeout);
    if (poll_timeout == -2) return -1;
    if (readfds != 0) original_read = *readfds;
    if (writefds != 0) original_write = *writefds;
    if (exceptfds != 0) original_except = *exceptfds;
    for (descriptor = 0; descriptor < nfds; ++descriptor) {
        short events = 0;
        if (readfds != 0 && FD_ISSET(descriptor, &original_read)) events |= POLLIN;
        if (writefds != 0 && FD_ISSET(descriptor, &original_write)) events |= POLLOUT;
        if (exceptfds != 0 && FD_ISSET(descriptor, &original_except)) events |= POLLPRI;
        if (events == 0) continue;
        pollfds[count].fd = descriptor;
        pollfds[count].events = events;
        pollfds[count].revents = 0;
        descriptors[count] = descriptor;
        ++count;
    }
    result = poll(pollfds, count, poll_timeout);
    if (result < 0) return -1;
    if (readfds != 0) FD_ZERO(readfds);
    if (writefds != 0) FD_ZERO(writefds);
    if (exceptfds != 0) FD_ZERO(exceptfds);
    for (nfds_t index = 0U; index < count; ++index) {
        short revents = pollfds[index].revents;
        descriptor = descriptors[index];
        if ((revents & POLLNVAL) != 0) {
            errno = EBADF;
            return -1;
        }
        if (readfds != 0 && (revents & (POLLIN | POLLERR | POLLHUP)) != 0) {
            FD_SET(descriptor, readfds);
        }
        if (writefds != 0 && (revents & (POLLOUT | POLLERR | POLLHUP)) != 0) {
            FD_SET(descriptor, writefds);
        }
        if (exceptfds != 0 && (revents & POLLPRI) != 0) {
            FD_SET(descriptor, exceptfds);
        }
    }
    for (descriptor = 0; descriptor < nfds; ++descriptor) {
        if ((readfds != 0 && FD_ISSET(descriptor, readfds)) ||
            (writefds != 0 && FD_ISSET(descriptor, writefds)) ||
            (exceptfds != 0 && FD_ISSET(descriptor, exceptfds))) {
            ++ready;
        }
    }
    return ready;
}
