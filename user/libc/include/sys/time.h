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

int gettimeofday(struct timeval *value, struct timezone *zone);
int settimeofday(const struct timeval *value, const struct timezone *zone);
