#pragma once

#include <liteos/libc.h>
#include <sys/types.h>

struct timespec {
    time_t tv_sec;
    long tv_nsec;
};

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

typedef int clockid_t;
#define CLOCK_REALTIME ((clockid_t)OS_CLOCK_REALTIME)
#define CLOCK_MONOTONIC ((clockid_t)OS_CLOCK_MONOTONIC)
#define TIME_UTC 1
#define TIMER_ABSTIME 1

time_t time(time_t *result);
clock_t clock(void);
int clock_gettime(clockid_t clock_id, struct timespec *value);
int clock_settime(clockid_t clock_id, const struct timespec *value);
int clock_getres(clockid_t clock_id, struct timespec *value);
int timespec_get(struct timespec *value, int base);
int nanosleep(const struct timespec *request, struct timespec *remaining);
int clock_nanosleep(clockid_t clock_id, int flags,
                    const struct timespec *request,
                    struct timespec *remaining);
double difftime(time_t end, time_t start);
struct tm *gmtime(const time_t *value);
struct tm *localtime(const time_t *value);
struct tm *gmtime_r(const time_t *value, struct tm *result);
struct tm *localtime_r(const time_t *value, struct tm *result);
time_t mktime(struct tm *value);
char *asctime(const struct tm *value);
char *asctime_r(const struct tm *value, char *buffer);
char *ctime(const time_t *value);
char *ctime_r(const time_t *value, char *buffer);
size_t strftime(char *buffer, size_t capacity, const char *format,
                const struct tm *value);
