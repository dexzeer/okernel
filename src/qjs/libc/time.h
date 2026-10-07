#ifndef QJS_TIME_H
#define QJS_TIME_H
#include <stddef.h>

typedef long long time_t;
typedef long clock_t;
typedef int clockid_t;

struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
    long tm_gmtoff;
    const char* tm_zone;
};

struct timespec { time_t tv_sec; long tv_nsec; };

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

int        clock_gettime(clockid_t id, struct timespec* ts);
time_t     time(time_t* t);
struct tm* gmtime(const time_t* t);
struct tm* gmtime_r(const time_t* t, struct tm* out);
struct tm* localtime(const time_t* t);
struct tm* localtime_r(const time_t* t, struct tm* out);
time_t     mktime(struct tm* tm);

#endif
