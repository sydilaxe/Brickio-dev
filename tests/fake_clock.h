/* the tests' clock: CLOCK_MONOTONIC as the test sets it (fake_us), for code that reads the time itself */
#pragma once
#include <time.h>
extern unsigned long long fake_us;
static inline int fake_clock_gettime(clockid_t c, struct timespec *ts)
{
    (void)c;
    ts->tv_sec = (time_t)(fake_us / 1000000ULL);
    ts->tv_nsec = (long)(fake_us % 1000000ULL) * 1000;
    return 0;
}
#define clock_gettime fake_clock_gettime
