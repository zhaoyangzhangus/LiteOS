#pragma once

#include <sys/types.h>

struct timeval {
    time_t tv_sec;
    long tv_usec;
};

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

#ifndef timerisset
#define timerisset(value) ((value)->tv_sec != 0 || (value)->tv_usec != 0)
#endif
#ifndef timerclear
#define timerclear(value) ((value)->tv_sec = 0, (value)->tv_usec = 0)
#endif
#ifndef timercmp
#define timercmp(left, right, comparison)                                      \
    (((left)->tv_sec == (right)->tv_sec)                                      \
         ? ((left)->tv_usec comparison (right)->tv_usec)                     \
         : ((left)->tv_sec comparison (right)->tv_sec))
#endif

#ifdef __cplusplus
extern "C" {
#endif
int gettimeofday(struct timeval *value, struct timezone *zone);
int settimeofday(const struct timeval *value, const struct timezone *zone);
int utimes(const char *path, const struct timeval times[2]);
int futimes(int descriptor, const struct timeval times[2]);
#ifdef __cplusplus
}
#endif
