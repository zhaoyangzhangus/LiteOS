#pragma once

#include <fcntl.h>
#include <time.h>

#define TFD_CLOEXEC  O_CLOEXEC
#define TFD_NONBLOCK O_NONBLOCK
#define TFD_TIMER_ABSTIME 1

int timerfd_create(clockid_t clock_id, int flags);
int timerfd_settime(int descriptor, int flags,
                    const struct itimerspec *new_value,
                    struct itimerspec *old_value);
int timerfd_gettime(int descriptor, struct itimerspec *value);
